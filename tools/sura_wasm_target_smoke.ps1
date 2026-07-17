param(
    [string]$Transpiler = (Join-Path $PSScriptRoot "sura_to_wasm.ps1"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "test_wasm_target.sura"),
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_wasm_target_" + [System.Guid]::NewGuid().ToString("N"))

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
    throw "Sura engine not found for AST JSON WASM smoke"
}

try {
    if (-not (Test-Path -LiteralPath $Transpiler)) {
        throw "Sura WASM transpiler not found: $Transpiler"
    }
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Sura WASM target source not found: $Source"
    }

    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $wat = Join-Path $temp "test_wasm_target.wat"
    $wasm = Join-Path $temp "test_wasm_target.wasm"
    $ps = Get-PowerShellRunner
    $enginePath = Resolve-SuraEngine $Engine
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $Source -Out $wat -WasmOut $wasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wasm)) {
        throw "sura_to_wasm WAT validation/binary emission failed with exit code $LASTEXITCODE"
    }

    $watText = [System.IO.File]::ReadAllText($wat, [System.Text.Encoding]::ASCII)
    if ($watText -notmatch 'AST JSON input: sura\.ast\.v1 \(exported from Sura source\)' -or
        $watText -match 'Source input: recursive Sura source expansion') {
        throw "generated WAT should lower .sura input through the AST JSON frontend instead of the legacy source-line frontend"
    }

    $dynamicStringDispatchSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_dynamic_string_function_dispatch.sura"
    $dynamicStringDispatchWat = Join-Path $temp "wasm_dynamic_string_function_dispatch.wat"
    $dynamicStringDispatchWasm = Join-Path $temp "wasm_dynamic_string_function_dispatch.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $dynamicStringDispatchSource -Out $dynamicStringDispatchWat -WasmOut $dynamicStringDispatchWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dynamicStringDispatchWasm)) {
        throw "dynamic string function dispatch WASM emission failed with exit code $LASTEXITCODE"
    }
    $dynamicStringDispatchWatText = [System.IO.File]::ReadAllText($dynamicStringDispatchWat, [System.Text.Encoding]::ASCII)
    foreach ($prefixName in @("prefix_a", "prefix_b")) {
        $prefixStart = $dynamicStringDispatchWatText.IndexOf("(func `$$prefixName")
        if ($prefixStart -lt 0) {
            throw "dynamic string function dispatch WAT should include $prefixName"
        }
        $prefixNext = $dynamicStringDispatchWatText.IndexOf("  (func ", $prefixStart + 1)
        if ($prefixNext -lt 0) { $prefixNext = $dynamicStringDispatchWatText.Length }
        $prefixBody = $dynamicStringDispatchWatText.Substring($prefixStart, $prefixNext - $prefixStart)
        if ($prefixBody -match 'call \$__sura_i32_to_string' -or
            $prefixBody -notmatch 'local\.get \$value\s+(?:call \$__sura_value_to_string\s+)?call \$__sura_string_concat') {
            throw "$prefixName should concatenate its string or tagged Value parameter without numeric stringification"
        }
    }
    $wasmStringRunner = Join-Path $temp "wasm_main_string_expect.js"
    [System.IO.File]::WriteAllText($wasmStringRunner, @'
const fs = require("fs");
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  const expected = Buffer.from(process.argv[3], "base64").toString("utf8");
  const label = process.argv[4] || "WASM main";
  const ptr = instance.exports.main();
  const view = new DataView(instance.exports.memory.buffer);
  if (!Number.isInteger(ptr) || ptr < 4 || ptr > view.byteLength) {
    throw new Error(`${label} returned invalid Sura string pointer ${ptr}`);
  }
  const length = view.getInt32(ptr - 4, true);
  if (length < 0 || ptr + length * 4 > view.byteLength) {
    throw new Error(`${label} returned invalid Sura string length ${length}`);
  }
  const codePoints = [];
  for (let i = 0; i < length; i += 1) codePoints.push(view.getInt32(ptr + i * 4, true));
  const got = String.fromCodePoint(...codePoints);
  if (got !== expected) throw new Error(`${label} expected ${JSON.stringify(expected)}, got ${JSON.stringify(got)}`);
}).catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
'@, [System.Text.Encoding]::UTF8)
    $node = Get-Command node -ErrorAction Stop
    $dynamicStringExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes("Ax|Bx"))
    & $node.Source $wasmStringRunner $dynamicStringDispatchWasm $dynamicStringExpectedBase64 "dynamic string dispatch"
    if ($LASTEXITCODE -ne 0) {
        throw "dynamic string function dispatch WASM execution failed"
    }

    $mixedFunctionValueSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_mixed_function_value_call.sura"
    $mixedFunctionValueWat = Join-Path $temp "wasm_mixed_function_value_call.wat"
    $mixedFunctionValueWasm = Join-Path $temp "wasm_mixed_function_value_call.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $mixedFunctionValueSource -Out $mixedFunctionValueWat -WasmOut $mixedFunctionValueWasm -Engine $enginePath | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $mixedFunctionValueWasm)) {
        throw "mixed function-or-nil Value call WASM emission failed with exit code $LASTEXITCODE"
    }
    $mixedFunctionValueWatText = [System.IO.File]::ReadAllText($mixedFunctionValueWat, [System.Text.Encoding]::ASCII)
    $mixedFunctionMainStart = $mixedFunctionValueWatText.IndexOf('(func (export "main")')
    if ($mixedFunctionMainStart -lt 0) {
        throw "mixed function-or-nil Value call WAT should include main"
    }
    $mixedFunctionMainNext = $mixedFunctionValueWatText.IndexOf("  (func ", $mixedFunctionMainStart + 1)
    if ($mixedFunctionMainNext -lt 0) { $mixedFunctionMainNext = $mixedFunctionValueWatText.Length }
    $mixedFunctionMainBody = $mixedFunctionValueWatText.Substring($mixedFunctionMainStart, $mixedFunctionMainNext - $mixedFunctionMainStart)
    if (([regex]::Matches($mixedFunctionMainBody, 'call \$__sura_call_function_1')).Count -lt 6 -or
        $mixedFunctionMainBody -match 'call \$(add_one|double_value)' -or
        $mixedFunctionMainBody -notmatch '(?s)i32\.const 41\s+call \$__sura_value_num\s+call \$__sura_call_function_1' -or
        $mixedFunctionMainBody -notmatch '(?s)f64\.const 1\.5\s+call \$__sura_value_num_f64\s+call \$__sura_call_function_1' -or
        $mixedFunctionMainBody -notmatch '(?s)f64\.const 2147483648\s+call \$__sura_value_num_f64\s+call \$__sura_call_function_1') {
        throw "runtime-selected function-or-nil locals should use tagged-Value indirect unary dispatch for integer, fractional, and out-of-i32 arguments instead of a stale direct/raw function hint"
    }
    $mixedFunctionDispatchStart = $mixedFunctionValueWatText.IndexOf('(func $__sura_call_function_1')
    if ($mixedFunctionDispatchStart -lt 0) {
        throw "mixed function-or-nil Value call WAT should include the unary dispatcher"
    }
    $mixedFunctionDispatchNext = $mixedFunctionValueWatText.IndexOf("  (func ", $mixedFunctionDispatchStart + 1)
    if ($mixedFunctionDispatchNext -lt 0) { $mixedFunctionDispatchNext = $mixedFunctionValueWatText.Length }
    $mixedFunctionDispatchBody = $mixedFunctionValueWatText.Substring($mixedFunctionDispatchStart, $mixedFunctionDispatchNext - $mixedFunctionDispatchStart)
    if ($mixedFunctionDispatchBody -notmatch '(?s)local\.get \$arg0\s+call \$add_one\s+return' -or
        $mixedFunctionDispatchBody -notmatch '(?s)local\.get \$arg0\s+call \$double_value\s+return' -or
        $mixedFunctionDispatchBody -notmatch 'call \$__sura_raise_runtime_error' -or
        $mixedFunctionDispatchBody -match '\bunreachable\b') {
        throw "the unary dispatcher should preserve tagged arguments/results for reachable function values and raise a catchable Sura runtime error for invalid ids"
    }
    if ($mixedFunctionValueWatText -notmatch '(?s)\(func \$add_one.*?local\.get \$value\s+i32\.const 1\s+call \$__sura_value_num\s+call \$__sura_value_add' -or
        $mixedFunctionValueWatText -notmatch '(?s)\(func \$double_value.*?local\.get \$value\s+i32\.const 2\s+call \$__sura_value_num\s+call \$__sura_value_mul') {
        throw "functions that escape as runtime values should use the stable tagged-Value parameter ABI"
    }
    $mixedFunctionExpected = "mixed function value call ok"
    $mixedFunctionExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($mixedFunctionExpected))
    & $node.Source $wasmStringRunner $mixedFunctionValueWasm $mixedFunctionExpectedBase64 "mixed function-or-nil Value call"
    if ($LASTEXITCODE -ne 0) {
        throw "mixed function-or-nil Value call WASM execution failed"
    }
    $mixedFunctionVmOutput = (& $enginePath $mixedFunctionValueSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $mixedFunctionVmOutput.Trim() -ne $mixedFunctionExpected) {
        throw "mixed function-or-nil Value call VM parity failed: $mixedFunctionVmOutput"
    }
    $mixedFunctionJitOutput = (& $enginePath --jit $mixedFunctionValueSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or -not $mixedFunctionJitOutput.TrimStart().StartsWith($mixedFunctionExpected)) {
        throw "mixed function-or-nil Value call JIT parity failed: $mixedFunctionJitOutput"
    }

    $functionValueRegressionCases = @(
        @{
            Slug = "returned_param_function_capture"
            File = "wasm_returned_param_function_capture.sura"
            Expected = "returned parameter function capture ok"
            Label = "returned parameter function capture"
            Patterns = @('\(func \$__sura_func_expr_', 'call \$bump_capture_value')
        },
        @{
            Slug = "array_lookup_returned_function"
            File = "wasm_array_lookup_returned_function.sura"
            Expected = "array lookup returned function ok"
            Label = "array and method lookup returned function"
            Patterns = @('call \$__sura_value_require_function_id', 'call \$__sura_call_function_2')
        },
        @{
            Slug = "tagged_dynamic_index"
            File = "wasm_tagged_dynamic_index.sura"
            Expected = "tagged dynamic index ok"
            Label = "tagged numeric index and range match"
            Patterns = @('call \$__sura_value_as_i32', 'call \$__sura_string_at', 'call \$__sura_array_get_checked')
        }
    )
    foreach ($regressionCase in $functionValueRegressionCases) {
        $regressionSource = Join-Path (Split-Path -Parent $PSScriptRoot) ("tests/" + [string]$regressionCase.File)
        $regressionWat = Join-Path $temp (([string]$regressionCase.Slug) + ".wat")
        $regressionWasm = Join-Path $temp (([string]$regressionCase.Slug) + ".wasm")
        & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $regressionSource -Out $regressionWat -WasmOut $regressionWasm -Engine $enginePath | Out-Host
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $regressionWasm)) {
            throw "$([string]$regressionCase.Label) WASM emission failed with exit code $LASTEXITCODE"
        }
        $regressionWatText = [System.IO.File]::ReadAllText($regressionWat, [System.Text.Encoding]::ASCII)
        foreach ($pattern in @($regressionCase.Patterns)) {
            if ($regressionWatText -notmatch [string]$pattern) {
                throw "$([string]$regressionCase.Label) WAT is missing required lowering evidence: $pattern"
            }
        }
        $regressionExpected = [string]$regressionCase.Expected
        $regressionExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($regressionExpected))
        & $node.Source $wasmStringRunner $regressionWasm $regressionExpectedBase64 ([string]$regressionCase.Label)
        if ($LASTEXITCODE -ne 0) {
            throw "$([string]$regressionCase.Label) WASM execution failed"
        }
        $regressionVmOutput = (& $enginePath $regressionSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $regressionVmOutput.Trim() -ne $regressionExpected) {
            throw "$([string]$regressionCase.Label) VM parity failed: $regressionVmOutput"
        }
        $regressionJitOutput = (& $enginePath --jit $regressionSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
        if ($LASTEXITCODE -ne 0 -or -not $regressionJitOutput.TrimStart().StartsWith($regressionExpected)) {
            throw "$([string]$regressionCase.Label) JIT parity failed: $regressionJitOutput"
        }
    }

    $membershipSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_membership_operator.sura"
    $membershipWat = Join-Path $temp "wasm_membership_operator.wat"
    $membershipWasm = Join-Path $temp "wasm_membership_operator.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $membershipSource -Out $membershipWat -WasmOut $membershipWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $membershipWasm)) {
        throw "membership operator WASM emission failed with exit code $LASTEXITCODE"
    }
    $membershipWatText = [System.IO.File]::ReadAllText($membershipWat, [System.Text.Encoding]::ASCII)
    if ($membershipWatText -notmatch '\(func \$__sura_value_in ' -or
        $membershipWatText -notmatch '\(func \$__sura_value_raw_array_contains_typed ' -or
        $membershipWatText -notmatch '(?s)i32\.const 3\s+call \$__sura_value_in') {
        throw "membership operator should use tagged runtime dispatch and typed raw string-array equality"
    }
    $membershipExpected = "true|true|true|true|true|true|true|nil|false"
    $membershipExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($membershipExpected))
    & $node.Source $wasmStringRunner $membershipWasm $membershipExpectedBase64 "membership operator"
    if ($LASTEXITCODE -ne 0) {
        throw "membership operator WASM execution failed"
    }

    $arrayAddSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_array_addition.sura"
    $arrayAddWat = Join-Path $temp "wasm_array_addition.wat"
    $arrayAddWasm = Join-Path $temp "wasm_array_addition.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $arrayAddSource -Out $arrayAddWat -WasmOut $arrayAddWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $arrayAddWasm)) {
        throw "array addition WASM emission failed with exit code $LASTEXITCODE"
    }
    $arrayAddWatText = [System.IO.File]::ReadAllText($arrayAddWat, [System.Text.Encoding]::ASCII)
    if ($arrayAddWatText -notmatch 'call \$__sura_array_concat2' -or
        $arrayAddWatText -notmatch 'call \$__sura_value_add' -or
        $arrayAddWatText -notmatch '\(func \$__sura_value_pack_meta ' -or
        $arrayAddWatText -notmatch '\(func \$__sura_value_array_typed ' -or
        $arrayAddWatText -notmatch '\(func \$__sura_value_meta ' -or
        $arrayAddWatText -notmatch 'call \$__sura_array_to_dynamic_values' -or
        $arrayAddWatText -notmatch '(?s)\(func \$append_dynamic_array.*?i32\.const 1\s+call \$__sura_value_array_typed.*?call \$__sura_value_add' -or
        $arrayAddWatText -notmatch '(?s)\(func \$prepend_dynamic_array.*?i32\.const 1\s+call \$__sura_value_array_typed.*?call \$__sura_value_add' -or
        $arrayAddWatText -notmatch '(?s)local\.get \$raw_metadata_numbers.*?i32\.const 1\s+call \$__sura_value_array_typed.*?call \$choose_array_or_bool.*?local\.get \$runtime_selected_numbers.*?call \$dynamic_array_add' -or
        $arrayAddWatText -notmatch '(?s)call \$__sura_array_slice.*?i32\.const 1\s+call \$__sura_value_array_typed.*?call \$choose_array_or_bool.*?local\.get \$runtime_selected_slice.*?call \$dynamic_array_add' -or
        $arrayAddWatText -notmatch '(?s)(?:call \$__sura_array_slice.*?){12}i32\.const 1\s+call \$__sura_value_array_typed.*?call \$choose_array_or_bool.*?local\.set \$runtime_selected_deep_slice.*?call \$dynamic_array_add' -or
        $arrayAddWatText -notmatch '(?s)call \$__sura_value_add\s+return') {
        throw "array addition should preserve raw-array element metadata through arbitrarily deep direct producer chains and normalize tag-4/tag-8 pairs inside tagged Value addition"
    }
    $arrayAddExpected = '[1, 2, 3, 4]|["su", "ra"]|[5, 6, 7]|["wasm", "value"]|nil|[8, 9, 10]|nil|[11, 12, 13]|nil|[14, 15, 16]|nil|[17, 18, 20]|[21, 22, 23]'
    $arrayAddExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($arrayAddExpected))
    & $node.Source $wasmStringRunner $arrayAddWasm $arrayAddExpectedBase64 "array addition"
    if ($LASTEXITCODE -ne 0) {
        throw "array addition WASM execution failed"
    }

    $numericErrorSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_dynamic_numeric_errors.sura"
    $numericErrorWat = Join-Path $temp "wasm_dynamic_numeric_errors.wat"
    $numericErrorWasm = Join-Path $temp "wasm_dynamic_numeric_errors.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $numericErrorSource -Out $numericErrorWat -WasmOut $numericErrorWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $numericErrorWasm)) {
        throw "dynamic numeric error WASM emission failed with exit code $LASTEXITCODE"
    }
    $numericErrorWatText = [System.IO.File]::ReadAllText($numericErrorWat, [System.Text.Encoding]::ASCII)
    if ($numericErrorWatText -notmatch '\(func \$__sura_raise_runtime_error ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_i32_div ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_i32_mod ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_i32_shl ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_i32_shr ' -or
        $numericErrorWatText -notmatch '(?s)\(func \$__sura_value_require_num.*?call \$__sura_raise_runtime_error.*?return' -or
        $numericErrorWatText -notmatch '\(func \$__sura_value_require_safe_i64 ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_value_box_safe_i64 ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_value_bitwise ' -or
        $numericErrorWatText -notmatch '\(func \$__sura_value_bitwise_not ' -or
        $numericErrorWatText -notmatch '(?s)\(func \$__sura_value_require_safe_i64.*?f64\.trunc.*?f64\.const -9007199254740991.*?f64\.const 9007199254740991.*?i64\.trunc_f64_s' -or
        $numericErrorWatText -notmatch '(?s)\(func \$__sura_value_bitwise.*?i64\.and.*?i64\.or.*?i64\.xor.*?i64\.shr_u.*?i64\.shl.*?i64\.shr_s.*?call \$__sura_value_box_safe_i64' -or
        $numericErrorWatText -notmatch '(?s)\(func \$dynamic_divide.*?call \$__sura_value_div\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown' -or
        $numericErrorWatText -notmatch '(?s)\(func \$dynamic_shift_left.*?i32\.const 3\s+call \$__sura_value_bitwise\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown' -or
        $numericErrorWatText -notmatch '(?s)\(func \$dynamic_bit_not.*?call \$__sura_value_bitwise_not\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown' -or
        $numericErrorWatText -notmatch '(?s)f64\.const 1\.5\s+call \$__sura_value_num_f64\s+i32\.const 1\s+call \$__sura_value_num\s+i32\.const 1\s+call \$__sura_value_bitwise' -or
        $numericErrorWatText -notmatch 'br \$__try_end\d+') {
        throw "dynamic numeric operations should preserve real division plus safe-i64 bitwise/shift results and convert type, fractional, range, zero-divisor, and invalid-shift failures into the catchable WASM exception side channel"
    }
    $numericErrorExpected = "dynamic numeric errors ok"
    $numericErrorExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($numericErrorExpected))
    & $node.Source $wasmStringRunner $numericErrorWasm $numericErrorExpectedBase64 "dynamic numeric errors"
    if ($LASTEXITCODE -ne 0) {
        throw "dynamic numeric error WASM execution failed"
    }
    $numericVmOutput = (& $enginePath $numericErrorSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $numericVmOutput -notmatch [regex]::Escape($numericErrorExpected)) {
        throw "dynamic numeric error VM parity failed: $numericVmOutput"
    }
    $numericJitOutput = (& $enginePath --jit $numericErrorSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $numericJitOutput -notmatch [regex]::Escape($numericErrorExpected)) {
        throw "dynamic numeric error JIT-entry parity failed: $numericJitOutput"
    }

    $directLargeCallSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_direct_large_literal_call.sura"
    $directLargeCallWat = Join-Path $temp "wasm_direct_large_literal_call.wat"
    $directLargeCallWasm = Join-Path $temp "wasm_direct_large_literal_call.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $directLargeCallSource -Out $directLargeCallWat -WasmOut $directLargeCallWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $directLargeCallWasm)) {
        throw "direct large-literal user call WASM emission failed with exit code $LASTEXITCODE"
    }
    $directLargeCallWatText = [System.IO.File]::ReadAllText($directLargeCallWat, [System.Text.Encoding]::ASCII)
    if ($directLargeCallWatText -notmatch '(?s)f64\.const 2147483648\s+call \$__sura_value_num_f64\s+call \$large_identity' -or
        $directLargeCallWatText -notmatch '(?s)f64\.const -2147483649\s+call \$__sura_value_num_f64\s+call \$large_identity' -or
        $directLargeCallWatText -notmatch '(?s)f64\.const 9007199254740991\s+call \$__sura_value_num_f64\s+call \$large_identity' -or
        $directLargeCallWatText -notmatch '(?s)f64\.const 2147483648\s+call \$__sura_value_num_f64\s+call \$large_add_one' -or
        $directLargeCallWatText -notmatch '(?s)\(func \$large_add_one.*?local\.get \$value\s+i32\.const 1\s+call \$__sura_value_num\s+call \$__sura_value_add') {
        throw "direct user-function calls should pass fractional-storage literals through the observed tagged Value ABI without an unused raw-i32 probe"
    }
    $directLargeCallExpected = "direct large literal call ok"
    $directLargeCallExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($directLargeCallExpected))
    & $node.Source $wasmStringRunner $directLargeCallWasm $directLargeCallExpectedBase64 "direct large-literal user call"
    if ($LASTEXITCODE -ne 0) {
        throw "direct large-literal user call WASM execution failed"
    }
    $directLargeCallVmOutput = (& $enginePath $directLargeCallSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $directLargeCallVmOutput.Trim() -ne $directLargeCallExpected) {
        throw "direct large-literal user call VM parity failed: $directLargeCallVmOutput"
    }
    $directLargeCallJitOutput = (& $enginePath --jit $directLargeCallSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or -not $directLargeCallJitOutput.TrimStart().StartsWith($directLargeCallExpected)) {
        throw "direct large-literal user call JIT-entry parity failed: $directLargeCallJitOutput"
    }

    $realDivisionSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_real_division.sura"
    $realDivisionWat = Join-Path $temp "wasm_real_division.wat"
    $realDivisionWasm = Join-Path $temp "wasm_real_division.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $realDivisionSource -Out $realDivisionWat -WasmOut $realDivisionWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $realDivisionWasm)) {
        throw "real division WASM emission failed with exit code $LASTEXITCODE"
    }
    $realDivisionWatText = [System.IO.File]::ReadAllText($realDivisionWat, [System.Text.Encoding]::ASCII)
    if ($realDivisionWatText -notmatch '(?s)\(func \$ratio.*?call \$__sura_value_div' -or
        $realDivisionWatText -notmatch '(?s)\(func \$half.*?call \$__sura_value_div' -or
        $realDivisionWatText -notmatch '(?s)\(func \$__sura_value_numeric_arith.*?i32\.const 2\s+i32\.eq\s+if.*?f64\.convert_i32_s.*?f64\.convert_i32_s.*?f64\.div\s+call \$__sura_value_num_f64' -or
        $realDivisionWatText -notmatch '(?s)local\.get \$in_place\s+call \$__sura_value_num\s+local\.set \$__sura_wasm_value_tmp.*?call \$__sura_value_div.*?local\.set \$in_place' -or
        $realDivisionWatText -match '(?s)\(func \$ratio.*?call \$__sura_i32_div') {
        throw "integer-looking Sura division should lower to tagged real f64 division across direct, function, boundary, and in-place paths"
    }
    $realDivisionExpected = "real division ok"
    $realDivisionExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($realDivisionExpected))
    & $node.Source $wasmStringRunner $realDivisionWasm $realDivisionExpectedBase64 "real division"
    if ($LASTEXITCODE -ne 0) {
        throw "real division WASM execution failed"
    }
    $realDivisionVmOutput = (& $enginePath $realDivisionSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $realDivisionVmOutput -ne $realDivisionExpected) {
        throw "real division VM parity failed: $realDivisionVmOutput"
    }
    $realDivisionJitOutput = (& $enginePath --jit $realDivisionSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $realDivisionJitOutput -notmatch ('^' + [regex]::Escape($realDivisionExpected))) {
        throw "real division JIT-entry parity failed: $realDivisionJitOutput"
    }

    $staticF64Source = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_static_f64_arithmetic.sura"
    $staticF64Wat = Join-Path $temp "wasm_static_f64_arithmetic.wat"
    $staticF64Wasm = Join-Path $temp "wasm_static_f64_arithmetic.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $staticF64Source -Out $staticF64Wat -WasmOut $staticF64Wasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $staticF64Wasm)) {
        throw "static f64 arithmetic WASM emission failed with exit code $LASTEXITCODE"
    }
    $staticF64WatText = [System.IO.File]::ReadAllText($staticF64Wat, [System.Text.Encoding]::ASCII)
    if ($staticF64WatText -notmatch '\(func \$__sura_value_math_unary ' -or
        $staticF64WatText -notmatch '\(func \$__sura_value_math_select ' -or
        $staticF64WatText -notmatch '\(func \$__sura_value_pow_integer ' -or
        $staticF64WatText -notmatch '\(func \$__sura_value_pow ' -or
        $staticF64WatText -notmatch '\(func \$__sura_f64_sin ' -or
        $staticF64WatText -notmatch '\(func \$__sura_f64_cos ' -or
        $staticF64WatText -notmatch '\(func \$__sura_f64_log_positive ' -or
        $staticF64WatText -notmatch '\(func \$__sura_f64_exp ' -or
        $staticF64WatText -notmatch '(?s)\(func \$static_sum.*?f64\.const 1\.5\s+f64\.const 2\.25\s+f64\.add\s+call \$__sura_value_num_f64\s+return' -or
        $staticF64WatText -notmatch '(?s)\(func \$static_product.*?f64\.const 1\.5\s+f64\.const 0\.5\s+f64\.add\s+f64\.const 2\.25\s+f64\.mul\s+call \$__sura_value_num_f64\s+return' -or
        $staticF64WatText -notmatch '(?s)\(func \$__sura_value_math_unary.*?f64\.abs.*?f64\.sqrt.*?f64\.floor.*?f64\.ceil.*?f64\.add.*?f64\.sub' -or
        $staticF64WatText -notmatch '(?s)\(func \$__sura_value_math_select.*?f64\.lt.*?f64\.gt.*?call \$__sura_value_num_f64' -or
        $staticF64WatText -notmatch '(?s)\(func \$__sura_value_pow_integer.*?f64\.trunc.*?i32\.trunc_f64_s.*?f64\.mul.*?f64\.div.*?call \$__sura_value_num_f64' -or
        $staticF64WatText -notmatch '(?s)\(func \$__sura_value_pow.*?f64\.trunc.*?call \$__sura_value_pow_integer.*?call \$__sura_f64_log_positive.*?call \$__sura_f64_exp.*?call \$__sura_value_num_f64' -or
        ([regex]::Matches($staticF64WatText, 'call \$__sura_value_math_unary').Count -lt 12) -or
        ([regex]::Matches($staticF64WatText, 'call \$__sura_value_math_select').Count -lt 6) -or
        ([regex]::Matches($staticF64WatText, 'call \$__sura_value_pow\b').Count -lt 4) -or
        ([regex]::Matches($staticF64WatText, 'call \$__sura_f64_sin\b').Count -lt 2) -or
        ([regex]::Matches($staticF64WatText, 'call \$__sura_f64_cos\b').Count -lt 2) -or
        $staticF64WatText -notmatch '(?s)call \$__sura_value_math_unary\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown.*?br \$__try_end\d+') {
        throw "static f64 +, -, and * should use raw f64 instructions, while runtime-selected unary/trigonometric math, min/max/clamp, and integer/fractional pow use guarded Value helpers"
    }
    $staticF64Expected = "static f64 arithmetic ok"
    $staticF64ExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($staticF64Expected))
    & $node.Source $wasmStringRunner $staticF64Wasm $staticF64ExpectedBase64 "static f64 arithmetic"
    if ($LASTEXITCODE -ne 0) {
        throw "static f64 arithmetic WASM execution failed"
    }
    $staticF64VmOutput = (& $enginePath $staticF64Source 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $staticF64VmOutput -ne $staticF64Expected) {
        throw "static f64 arithmetic VM parity failed: $staticF64VmOutput"
    }
    $staticF64JitOutput = (& $enginePath --jit $staticF64Source 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $staticF64JitOutput -notmatch ('^' + [regex]::Escape($staticF64Expected))) {
        throw "static f64 arithmetic JIT-entry parity failed: $staticF64JitOutput"
    }

    $rawF64ExportSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_raw_f64_export.sura"
    $rawF64ExportWat = Join-Path $temp "wasm_raw_f64_export.wat"
    $rawF64ExportWasm = Join-Path $temp "wasm_raw_f64_export.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $rawF64ExportSource -Out $rawF64ExportWat -WasmOut $rawF64ExportWasm -Engine $enginePath | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $rawF64ExportWasm)) {
        throw "raw f64 export WASM emission failed with exit code $LASTEXITCODE"
    }
    $rawF64ExportWatText = [System.IO.File]::ReadAllText($rawF64ExportWat, [System.Text.Encoding]::ASCII)
    if ($rawF64ExportWatText -notmatch '\(func \$__sura_raw_f64_export_raw_f64_affine \(export "raw_f64_affine__f64"\) \(param \$arg0 f64\) \(param \$arg1 f64\) \(result f64\)' -or
        $rawF64ExportWatText -notmatch '\(func \$__sura_raw_f64_export_raw_f64_ratio \(export "raw_f64_ratio__f64"\) \(param \$arg0 f64\) \(param \$arg1 f64\) \(result f64\)' -or
        $rawF64ExportWatText -notmatch '\(func \$__sura_raw_f64_export_raw_f64_constant \(export "raw_f64_constant__f64"\)\s+\(result f64\)' -or
        $rawF64ExportWatText -notmatch '\(func \$__sura_raw_f64_export_raw_f64_identity \(export "raw_f64_identity__f64"\) \(param \$arg0 f64\) \(result f64\)' -or
        ([regex]::Matches($rawF64ExportWatText, 'call \$__sura_value_num_f64').Count -lt 8) -or
        ([regex]::Matches($rawF64ExportWatText, 'call \$__sura_value_require_number_f64').Count -lt 8)) {
        throw "explicitly typed numeric functions should expose companion name__f64 exports with native f64 parameters and results"
    }
    $rawF64ExportRunner = Join-Path $temp "wasm_raw_f64_export.js"
    [System.IO.File]::WriteAllText($rawF64ExportRunner, @'
const fs = require("fs");
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  const e = instance.exports;
  const near = (left, right) => Math.abs(left - right) < 1e-12;
  if (!near(e.raw_f64_affine__f64(1.5, 2), 3.5) ||
      !near(e.raw_f64_ratio__f64(7, 2), 3.5) ||
      !near(e.raw_f64_constant__f64(), 3.75)) {
    throw new Error("raw f64 companion export arithmetic mismatch");
  }
  if (!Object.is(e.raw_f64_identity__f64(-0), -0) ||
      !Number.isNaN(e.raw_f64_identity__f64(NaN)) ||
      e.raw_f64_identity__f64(Infinity) !== Infinity) {
    throw new Error("raw f64 companion export lost an IEEE-754 value");
  }
}).catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
'@, [System.Text.Encoding]::UTF8)
    & $node.Source $rawF64ExportRunner $rawF64ExportWasm
    if ($LASTEXITCODE -ne 0) {
        throw "raw f64 companion export WASM execution failed"
    }
    $rawF64Expected = "raw f64 export ok"
    $rawF64ExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($rawF64Expected))
    & $node.Source $wasmStringRunner $rawF64ExportWasm $rawF64ExpectedBase64 "raw f64 Sura entry"
    if ($LASTEXITCODE -ne 0) {
        throw "raw f64 export Sura entry WASM execution failed"
    }
    $rawF64VmOutput = (& $enginePath $rawF64ExportSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $rawF64VmOutput -ne $rawF64Expected) {
        throw "raw f64 export VM parity failed: $rawF64VmOutput"
    }
    $rawF64JitOutput = (& $enginePath --jit $rawF64ExportSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $rawF64JitOutput -notmatch ('^' + [regex]::Escape($rawF64Expected))) {
        throw "raw f64 export JIT-entry parity failed: $rawF64JitOutput"
    }

    $decimalValueSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_dynamic_decimal_value.sura"
    $decimalValueWat = Join-Path $temp "wasm_dynamic_decimal_value.wat"
    $decimalValueWasm = Join-Path $temp "wasm_dynamic_decimal_value.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $decimalValueSource -Out $decimalValueWat -WasmOut $decimalValueWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $decimalValueWasm)) {
        throw "dynamic decimal Value WASM emission failed with exit code $LASTEXITCODE"
    }
    $decimalValueWatText = [System.IO.File]::ReadAllText($decimalValueWat, [System.Text.Encoding]::ASCII)
    if ($decimalValueWatText -notmatch '\(func \$__sura_value_num_f64 ' -or
        $decimalValueWatText -notmatch '\(func \$__sura_value_number_f64 ' -or
        $decimalValueWatText -notmatch 'f64\.store offset=4 align=4' -or
        $decimalValueWatText -notmatch 'f64\.load offset=4 align=4' -or
        $decimalValueWatText -notmatch '(?s)f64\.const 1\.5\s+call \$__sura_value_num_f64\s+call \$decimal_identity' -or
        $decimalValueWatText -notmatch '(?s)\(func \$__sura_value_eq.*?call \$__sura_value_number_f64.*?f64\.eq' -or
        $decimalValueWatText -notmatch '(?s)\(func \$__sura_value_is_truthy.*?i32\.const 10.*?call \$__sura_value_number_f64.*?f64\.ne') {
        throw "dynamic decimal Values should preserve f64 payloads across the tagged function ABI and support numeric equality, type, and truthiness"
    }
    $decimalValueExpected = "dynamic decimal value ok"
    $decimalValueExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($decimalValueExpected))
    & $node.Source $wasmStringRunner $decimalValueWasm $decimalValueExpectedBase64 "dynamic decimal Value"
    if ($LASTEXITCODE -ne 0) {
        throw "dynamic decimal Value WASM execution failed"
    }
    $decimalVmOutput = (& $enginePath $decimalValueSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalVmOutput -notmatch [regex]::Escape($decimalValueExpected)) {
        throw "dynamic decimal Value VM parity failed: $decimalVmOutput"
    }
    $decimalJitOutput = (& $enginePath --jit $decimalValueSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalJitOutput -notmatch [regex]::Escape($decimalValueExpected)) {
        throw "dynamic decimal Value JIT-entry parity failed: $decimalJitOutput"
    }

    $decimalArithmeticSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_dynamic_decimal_arithmetic.sura"
    $decimalArithmeticWat = Join-Path $temp "wasm_dynamic_decimal_arithmetic.wat"
    $decimalArithmeticWasm = Join-Path $temp "wasm_dynamic_decimal_arithmetic.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $decimalArithmeticSource -Out $decimalArithmeticWat -WasmOut $decimalArithmeticWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $decimalArithmeticWasm)) {
        throw "dynamic decimal arithmetic WASM emission failed with exit code $LASTEXITCODE"
    }
    $decimalArithmeticWatText = [System.IO.File]::ReadAllText($decimalArithmeticWat, [System.Text.Encoding]::ASCII)
    if ($decimalArithmeticWatText -notmatch '\(func \$__sura_value_numeric_arith ' -or
        $decimalArithmeticWatText -notmatch '\(func \$__sura_value_numeric_compare ' -or
        $decimalArithmeticWatText -notmatch '(?s)\(func \$__sura_value_add.*?call \$__sura_value_number_f64.*?f64\.add.*?call \$__sura_value_num_f64' -or
        $decimalArithmeticWatText -notmatch '(?s)\(func \$__sura_value_numeric_arith.*?f64\.sub.*?f64\.mul.*?f64\.div.*?f64\.trunc' -or
        $decimalArithmeticWatText -notmatch '(?s)\(func \$__sura_value_numeric_compare.*?f64\.lt.*?f64\.le.*?f64\.gt.*?f64\.ge' -or
        $decimalArithmeticWatText -notmatch '(?s)call \$__sura_value_add.*?call \$__sura_value_sub.*?call \$__sura_value_mul.*?call \$__sura_value_div.*?call \$__sura_value_mod' -or
        $decimalArithmeticWatText -notmatch '(?s)local\.set \$compound.*?call \$__sura_value_div.*?local\.set \$compound.*?call \$__sura_value_add.*?local\.set \$compound.*?call \$__sura_value_mul.*?local\.set \$compound.*?call \$__sura_value_sub.*?local\.set \$compound.*?call \$__sura_value_mod.*?local\.set \$compound' -or
        $decimalArithmeticWatText -notmatch '(?s)call \$__sura_value_numeric_compare\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown' -or
        $decimalArithmeticWatText -notmatch '(?s)call \$__sura_value_div\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown.*?br \$__try_end\d+') {
        throw "dynamic decimal arithmetic should preserve tagged f64 results for expressions and chained in-place +, -, *, /, % operations, comparisons, and catchable failures"
    }
    $decimalArithmeticExpected = "dynamic decimal arithmetic ok"
    $decimalArithmeticExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($decimalArithmeticExpected))
    & $node.Source $wasmStringRunner $decimalArithmeticWasm $decimalArithmeticExpectedBase64 "dynamic decimal arithmetic"
    if ($LASTEXITCODE -ne 0) {
        throw "dynamic decimal arithmetic WASM execution failed"
    }
    $decimalArithmeticVmOutput = (& $enginePath $decimalArithmeticSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalArithmeticVmOutput -notmatch [regex]::Escape($decimalArithmeticExpected)) {
        throw "dynamic decimal arithmetic VM parity failed: $decimalArithmeticVmOutput"
    }
    $decimalArithmeticJitOutput = (& $enginePath --jit $decimalArithmeticSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalArithmeticJitOutput -notmatch [regex]::Escape($decimalArithmeticExpected)) {
        throw "dynamic decimal arithmetic JIT-entry parity failed: $decimalArithmeticJitOutput"
    }

    $decimalStorageSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_decimal_storage.sura"
    $decimalStorageWat = Join-Path $temp "wasm_decimal_storage.wat"
    $decimalStorageWasm = Join-Path $temp "wasm_decimal_storage.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $decimalStorageSource -Out $decimalStorageWat -WasmOut $decimalStorageWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $decimalStorageWasm)) {
        throw "decimal storage WASM emission failed with exit code $LASTEXITCODE"
    }
    $decimalStorageWatText = [System.IO.File]::ReadAllText($decimalStorageWat, [System.Text.Encoding]::ASCII)
    if ($decimalStorageWatText -notmatch '(?s)\(func \$decimal_return.*?f64\.const 1\.5\s+call \$__sura_value_num_f64\s+return' -or
        $decimalStorageWatText -notmatch '(?s)\(func \$decimal_add_quarter \(export "decimal_add_quarter"\) \(param \$value i32\).*?local\.get \$value.*?f64\.const 0\.25\s+call \$__sura_value_num_f64\s+call \$__sura_value_add' -or
        $decimalStorageWatText -notmatch '(?s)f64\.const 1\.25\s+call \$__sura_value_num_f64.*?call \$__sura_value_dynamic_array' -or
        $decimalStorageWatText -notmatch '(?s)f64\.const 4\.5\s+call \$__sura_value_num_f64.*?call \$__sura_value_dynamic_dict' -or
        $decimalStorageWatText -notmatch '(?s)\(func \$__sura_value_to_string.*?i32\.const 10.*?f64\.load offset=4 align=4\s+call \$__sura_f64_to_string' -or
        $decimalStorageWatText -notmatch '(?s)f64\.const 1\.5\s+call \$__sura_value_num_f64\s+call \$__sura_value_to_string' -or
        $decimalStorageWatText -notmatch '(?s)local\.get \$local_decimal\s+call \$__sura_value_to_string.*?local\.get \$decimal_array\s+call \$__sura_value_to_string.*?local\.get \$decimal_dict\s+call \$__sura_value_to_string') {
        throw "fractional locals, returns, arguments, arrays, dictionaries, and stringification should use the tagged f64 Value ABI without i32 truncation"
    }
    $decimalStorageExpected = "decimal storage ok"
    $decimalStorageExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($decimalStorageExpected))
    & $node.Source $wasmStringRunner $decimalStorageWasm $decimalStorageExpectedBase64 "decimal storage"
    if ($LASTEXITCODE -ne 0) {
        throw "decimal storage WASM execution failed"
    }
    $decimalStorageVmOutput = (& $enginePath $decimalStorageSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalStorageVmOutput -notmatch [regex]::Escape($decimalStorageExpected)) {
        throw "decimal storage VM parity failed: $decimalStorageVmOutput"
    }
    $decimalStorageJitOutput = (& $enginePath --jit $decimalStorageSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalStorageJitOutput -notmatch [regex]::Escape($decimalStorageExpected)) {
        throw "decimal storage JIT-entry parity failed: $decimalStorageJitOutput"
    }

    $decimalConversionSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_decimal_conversion.sura"
    $decimalConversionWat = Join-Path $temp "wasm_decimal_conversion.wat"
    $decimalConversionWasm = Join-Path $temp "wasm_decimal_conversion.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $decimalConversionSource -Out $decimalConversionWat -WasmOut $decimalConversionWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $decimalConversionWasm)) {
        throw "decimal conversion WASM emission failed with exit code $LASTEXITCODE"
    }
    $decimalConversionWatText = [System.IO.File]::ReadAllText($decimalConversionWat, [System.Text.Encoding]::ASCII)
    if ($decimalConversionWatText -notmatch '(?s)f64\.const 3\.141592653589793\d*\s+call \$__sura_value_num_f64\s+local\.set \$pi_value' -or
        $decimalConversionWatText -notmatch '(?s)f64\.const 1\.5\s+f64\.const 2\.25\s+f64\.add\s+call \$__sura_value_num_f64\s+local\.set \$static_sum' -or
        $decimalConversionWatText -notmatch '(?s)f64\.const 6\.5\s+f64\.const 1\.25\s+f64\.sub\s+call \$__sura_value_num_f64\s+local\.set \$static_difference' -or
        $decimalConversionWatText -notmatch '(?s)f64\.const 1\.5\s+f64\.const 4\s+f64\.mul\s+call \$__sura_value_num_f64\s+local\.set \$static_product' -or
        $decimalConversionWatText -notmatch '(?s)f64\.const 1\.25\s+f64\.const 2\s+f64\.add\s+f64\.const 5\.5\s+f64\.const 1\.5\s+f64\.sub\s+f64\.mul\s+call \$__sura_value_num_f64\s+local\.set \$static_nested' -or
        $decimalConversionWatText -notmatch '(?s)\(func \$keep_float \(export "keep_float"\) \(param \$value i32\).*?local\.get \$value\s+call \$__sura_value_to_float\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown' -or
        $decimalConversionWatText -notmatch '(?s)\(func \$__sura_value_as_i32.*?i32\.const 10.*?f64\.load offset=4 align=4\s+f64\.floor\s+i32\.trunc_sat_f64_s' -or
        $decimalConversionWatText -notmatch '\(global \$__sura_parse_success \(mut i32\)' -or
        $decimalConversionWatText -notmatch '\(func \$__sura_string_parse_i32 ' -or
        $decimalConversionWatText -notmatch '\(func \$__sura_string_parse_f64 ' -or
        $decimalConversionWatText -notmatch '(?s)\(func \$__sura_string_parse_f64.*?call \$__sura_string_prefix_ascii3_ci.*?f64\.const -inf.*?f64\.const inf.*?call \$__sura_string_prefix_ascii3_ci.*?f64\.const nan' -or
        $decimalConversionWatText -notmatch '(?s)\(func \$__sura_value_to_int_i32.*?call \$__sura_string_parse_i32.*?global\.get \$__sura_parse_success.*?call \$__sura_raise_runtime_error' -or
        $decimalConversionWatText -notmatch '(?s)\(func \$__sura_value_to_float.*?call \$__sura_string_parse_f64.*?global\.get \$__sura_parse_success.*?call \$__sura_value_num_f64' -or
        $decimalConversionWatText -notmatch '(?s)local\.get \$pi_value\s+call \$__sura_value_to_int_i32\s+local\.set \$__sura_wasm_call_tmp.*?local\.set \$pi_integer' -or
        $decimalConversionWatText -notmatch '(?s)call \$__sura_value_to_int_i32\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown.*?br \$__try_end\d+' -or
        $decimalConversionWatText -notmatch '(?s)call \$__sura_value_to_float\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown.*?br \$__try_end\d+' -or
        $decimalConversionWatText -notmatch '(?s)f64\.const 3\.141592653589793\d*\s+call \$__sura_value_num_f64\s+call \$__sura_value_to_string') {
        throw "decimal conversion should lower statically numeric +, -, and * through raw f64 operations, preserve other fractional values plus finite/nan/inf conversions, and keep failures catchable"
    }
    $decimalConversionExpected = "decimal conversion ok"
    $decimalConversionExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($decimalConversionExpected))
    & $node.Source $wasmStringRunner $decimalConversionWasm $decimalConversionExpectedBase64 "decimal conversion"
    if ($LASTEXITCODE -ne 0) {
        throw "decimal conversion WASM execution failed"
    }
    $decimalConversionVmOutput = (& $enginePath $decimalConversionSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalConversionVmOutput -notmatch [regex]::Escape($decimalConversionExpected)) {
        throw "decimal conversion VM parity failed: $decimalConversionVmOutput"
    }
    $decimalConversionJitOutput = (& $enginePath --jit $decimalConversionSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalConversionJitOutput -notmatch [regex]::Escape($decimalConversionExpected)) {
        throw "decimal conversion JIT-entry parity failed: $decimalConversionJitOutput"
    }

    $decimalFormatSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_decimal_format_boundaries.sura"
    $decimalFormatWat = Join-Path $temp "wasm_decimal_format_boundaries.wat"
    $decimalFormatWasm = Join-Path $temp "wasm_decimal_format_boundaries.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $decimalFormatSource -Out $decimalFormatWat -WasmOut $decimalFormatWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $decimalFormatWasm)) {
        throw "decimal format boundary WASM emission failed with exit code $LASTEXITCODE"
    }
    $decimalFormatWatText = [System.IO.File]::ReadAllText($decimalFormatWat, [System.Text.Encoding]::ASCII)
    if ($decimalFormatWatText -notmatch '\(func \$__sura_ryu_umul_hi ' -or
        $decimalFormatWatText -notmatch '\(func \$__sura_ryu_mul_shift ' -or
        $decimalFormatWatText -notmatch '\(func \$__sura_ryu_d2d ' -or
        $decimalFormatWatText -notmatch '\(data \(i32\.const 16\) ' -or
        $decimalFormatWatText -notmatch '\(global \$__heap \(mut i32\) \(i32\.const 10704\)\)' -or
        $decimalFormatWatText -notmatch 'f64\.const 123456789\.12345679' -or
        $decimalFormatWatText -match '__sura_ryu_probe_') {
        throw "decimal formatting should use the private pure-WASM Ryu runtime, reserved lookup-table memory, and cross-parser exact f64 constants"
    }
    $decimalFormatExpected = '0.1|0.30000000000000004|1.2345678901234567|1e-06|1e-05|999999.9999999999|1.2345678912345679e+08|-9876.543210987655|2147483648|9.223372036854776e+18|1e+20'
    $decimalFormatExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($decimalFormatExpected))
    & $node.Source $wasmStringRunner $decimalFormatWasm $decimalFormatExpectedBase64 "decimal format boundaries"
    if ($LASTEXITCODE -ne 0) {
        throw "decimal format boundary WASM execution failed"
    }
    $decimalFormatVmOutput = (& $enginePath $decimalFormatSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalFormatVmOutput -ne $decimalFormatExpected) {
        throw "decimal format boundary VM parity failed: $decimalFormatVmOutput"
    }
    $decimalFormatJitOutput = (& $enginePath --jit $decimalFormatSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalFormatJitOutput -notmatch ('^' + [regex]::Escape($decimalFormatExpected))) {
        throw "decimal format boundary JIT-entry parity failed: $decimalFormatJitOutput"
    }

    $decimalStringSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_decimal_stringification.sura"
    $decimalStringWat = Join-Path $temp "wasm_decimal_stringification.wat"
    $decimalStringWasm = Join-Path $temp "wasm_decimal_stringification.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $decimalStringSource -Out $decimalStringWat -WasmOut $decimalStringWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $decimalStringWasm)) {
        throw "decimal stringification WASM emission failed with exit code $LASTEXITCODE"
    }
    $decimalStringExpected = "decimal stringification ok"
    $decimalStringExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($decimalStringExpected))
    & $node.Source $wasmStringRunner $decimalStringWasm $decimalStringExpectedBase64 "decimal stringification"
    if ($LASTEXITCODE -ne 0) {
        throw "decimal stringification WASM execution failed"
    }
    $decimalStringVmOutput = (& $enginePath $decimalStringSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalStringVmOutput -ne $decimalStringExpected) {
        throw "decimal stringification VM parity failed: $decimalStringVmOutput"
    }
    $decimalStringJitOutput = (& $enginePath --jit $decimalStringSource 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $decimalStringJitOutput -notmatch ('^' + [regex]::Escape($decimalStringExpected))) {
        throw "decimal stringification JIT-entry parity failed: $decimalStringJitOutput"
    }

    $exceptionExpressionSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_exception_expression_propagation.sura"
    $exceptionExpressionWat = Join-Path $temp "wasm_exception_expression_propagation.wat"
    $exceptionExpressionWasm = Join-Path $temp "wasm_exception_expression_propagation.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $exceptionExpressionSource -Out $exceptionExpressionWat -WasmOut $exceptionExpressionWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exceptionExpressionWasm)) {
        throw "expression-wide exception propagation WASM emission failed with exit code $LASTEXITCODE"
    }
    $exceptionExpressionWatText = [System.IO.File]::ReadAllText($exceptionExpressionWat, [System.Text.Encoding]::ASCII)
    if ($exceptionExpressionWatText -notmatch '(?s)call \$relay_label\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?br \$__try_end\d+' -or
        $exceptionExpressionWatText -notmatch 'global\.get \$__sura_exception_value_tagged' -or
        $exceptionExpressionWatText -notmatch 'call \$__sura_value_field' -or
        $exceptionExpressionWatText -notmatch 'call \$__sura_value_object' -or
        $exceptionExpressionWatText -notmatch '(?s)\(func \$__sura_call_method_\d+_0 .*?call \$__sura_object_class_id.*?call \$__sura_method_WasmObjectErrorA_describe.*?call \$__sura_method_WasmObjectErrorB_describe') {
        throw "nested expression throws should bridge tagged payloads into the enclosing catch before the remaining expression executes"
    }
    $exceptionExpressionExpected = 'bin:F|arg|array-expr|dict-expr|ternary|condition|interp|index|return|array:[1, "two", true]|dict:dict:7|number:9|instance:A:7|instance:B:9'
    $exceptionExpressionExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($exceptionExpressionExpected))
    & $node.Source $wasmStringRunner $exceptionExpressionWasm $exceptionExpressionExpectedBase64 "expression-wide exception propagation"
    if ($LASTEXITCODE -ne 0) {
        throw "expression-wide exception propagation WASM execution failed"
    }

    $dynamicInterpolationSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_dynamic_interpolation_value.sura"
    $dynamicInterpolationWat = Join-Path $temp "wasm_dynamic_interpolation_value.wat"
    $dynamicInterpolationWasm = Join-Path $temp "wasm_dynamic_interpolation_value.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $dynamicInterpolationSource -Out $dynamicInterpolationWat -WasmOut $dynamicInterpolationWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dynamicInterpolationWasm)) {
        throw "dynamic Value interpolation WASM emission failed with exit code $LASTEXITCODE"
    }
    $dynamicInterpolationWatText = [System.IO.File]::ReadAllText($dynamicInterpolationWat, [System.Text.Encoding]::ASCII)
    if ($dynamicInterpolationWatText -notmatch '\(func \$__sura_function_to_string ' -or
        $dynamicInterpolationWatText -notmatch '\(func \$__sura_object_to_string ' -or
        $dynamicInterpolationWatText -notmatch '(?s)i32\.const 4\s+i32\.eq\s+if.*?call \$__sura_value_meta.*?call \$__sura_array_to_dynamic_values.*?call \$__sura_value_array_to_string' -or
        $dynamicInterpolationWatText -notmatch '(?s)i32\.const 6\s+i32\.eq\s+if.*?call \$__sura_object_to_string' -or
        $dynamicInterpolationWatText -notmatch '(?s)i32\.const 7\s+i32\.eq\s+if.*?call \$__sura_function_to_string') {
        throw "dynamic interpolation should stringify typed raw arrays, function Values, and registered object Values through the tagged Value ABI"
    }
    $dynamicInterpolationExpected = '42|sura|true|nil|[1, 2, 3]|[1, "two", true]|[<Instance WasmInterpBox>, <Func wasm_interp_named>]|{"label": "sura"}|<Func wasm_interp_named>|<Instance WasmInterpBox>'
    $dynamicInterpolationExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($dynamicInterpolationExpected))
    & $node.Source $wasmStringRunner $dynamicInterpolationWasm $dynamicInterpolationExpectedBase64 "dynamic Value interpolation"
    if ($LASTEXITCODE -ne 0) {
        throw "dynamic Value interpolation WASM execution failed"
    }

    $fullSourceExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes("32"))
    & $node.Source $wasmStringRunner $wasm $fullSourceExpectedBase64 "full source WASM main"
    if ($LASTEXITCODE -ne 0) {
        throw "full source WASM main execution failed"
    }

    $foreachStringSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_foreach_string_return.sura"
    $foreachStringWat = Join-Path $temp "wasm_foreach_string_return.wat"
    $foreachStringWasm = Join-Path $temp "wasm_foreach_string_return.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $foreachStringSource -Out $foreachStringWat -WasmOut $foreachStringWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $foreachStringWasm)) {
        throw "foreach string return WASM emission failed with exit code $LASTEXITCODE"
    }
    $foreachStringWatText = [System.IO.File]::ReadAllText($foreachStringWat, [System.Text.Encoding]::ASCII)
    if ($foreachStringWatText -notmatch '(?s)call \$first_label.*?local\.get \$result\s+call \$__sura_value_string(?:_or_nil)?' -or
        $foreachStringWatText -match '(?s)call \$first_label.*?local\.get \$result\s+call \$__sura_value_num') {
        throw "foreach string return should preserve its function-local array element type at the caller"
    }
    $foreachStringExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes("red"))
    & $node.Source $wasmStringRunner $foreachStringWasm $foreachStringExpectedBase64 "foreach string return"
    if ($LASTEXITCODE -ne 0) {
        throw "foreach string return WASM execution failed"
    }

    $nestedCollectionStringSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_nested_collection_to_str.sura"
    $nestedCollectionStringWat = Join-Path $temp "wasm_nested_collection_to_str.wat"
    $nestedCollectionStringWasm = Join-Path $temp "wasm_nested_collection_to_str.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $nestedCollectionStringSource -Out $nestedCollectionStringWat -WasmOut $nestedCollectionStringWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nestedCollectionStringWasm)) {
        throw "nested collection to_str WASM emission failed with exit code $LASTEXITCODE"
    }
    $nestedCollectionExpected = '{"names": ["Sura", "WASM"], "profile": {"name": "Sura", "active": true}}'
    $nestedCollectionExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($nestedCollectionExpected))
    & $node.Source $wasmStringRunner $nestedCollectionStringWasm $nestedCollectionExpectedBase64 "nested collection to_str"
    if ($LASTEXITCODE -ne 0) {
        throw "nested collection to_str WASM execution failed"
    }

    $nestedAliasSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_nested_collection_alias_string.sura"
    $nestedAliasWat = Join-Path $temp "wasm_nested_collection_alias_string.wat"
    $nestedAliasWasm = Join-Path $temp "wasm_nested_collection_alias_string.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $nestedAliasSource -Out $nestedAliasWat -WasmOut $nestedAliasWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nestedAliasWasm)) {
        throw "nested collection alias string WASM emission failed with exit code $LASTEXITCODE"
    }
    $nestedAliasExpected = 'NestedCollections [["Sura", "WASM"], {"name": "Sura", "active": true}] {"names": ["Sura", "WASM"], "profile": {"name": "Sura", "active": true}}'
    $nestedAliasExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($nestedAliasExpected))
    & $node.Source $wasmStringRunner $nestedAliasWasm $nestedAliasExpectedBase64 "nested collection alias string"
    if ($LASTEXITCODE -ne 0) {
        throw "nested collection alias string WASM execution failed"
    }

    $dictReassignSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_local_dict_field_reassignment.sura"
    $dictReassignWat = Join-Path $temp "wasm_local_dict_field_reassignment.wat"
    $dictReassignWasm = Join-Path $temp "wasm_local_dict_field_reassignment.wasm"
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $dictReassignSource -Out $dictReassignWat -WasmOut $dictReassignWasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dictReassignWasm)) {
        throw "local dict field reassignment WASM emission failed with exit code $LASTEXITCODE"
    }
    $dictReassignWatText = [System.IO.File]::ReadAllText($dictReassignWat, [System.Text.Encoding]::ASCII)
    if ($dictReassignWatText -notmatch '(?s)local\.get \$profile.*?call \$__sura_make_array_\d+\s+local\.get \$profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const 5\s+i32\.add\s+call \$__sura_dict_put\s+local\.set \$profile') {
        throw "local dict field reassignment should refresh its local type hint before compound numeric assignment"
    }
    $dictReassignExpectedBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes("12"))
    & $node.Source $wasmStringRunner $dictReassignWasm $dictReassignExpectedBase64 "local dict field reassignment"
    if ($LASTEXITCODE -ne 0) {
        throw "local dict field reassignment WASM execution failed"
    }

    $globalSource = Join-Path $temp "global_decl.sura"
    $globalWat = Join-Path $temp "global_decl.wat"
@'
func global_value_handler() do
  return 1
end
score is 0
banner is "Sura"
numbers is [1, 2, 3]
items is [0, 1]
profile is {score: 4}
active_flag is false
missing_value is nil
handler_state is global_value_handler
state is nil
method_state is nil
ctor_state is nil
parent_state is nil
cond_state is nil
while_state is nil
repeat_state is nil
for_state is nil
foreach_state is nil
match_state is nil
throw_state is nil
dot_assign_state is nil
index_value_state is nil
index_key_state is nil
index_read_key_state is nil
index_read_numeric_state is nil
index_read_function_state is nil
index_to_str_state is nil
index_type_state is nil
index_length_state is nil
function_return_stringify_state is nil
method_return_stringify_state is nil
function_arg_return_stringify_state is nil
method_arg_return_stringify_state is nil
array_return_stringify_count is 0
dict_return_stringify_count is 0
func add_score() do
  global score
  global banner
  global numbers
  global items
  global profile
  score += 1
  banner += "!"
  numbers[1] += 5
  items[1] is "tail"
  profile.score += 6
  profile.bonus is 12
end
func read_score() do
  global score
  return score
end
func read_banner() do
  global banner
  return banner
end
func read_numbers() do
  global numbers
  return numbers
end
func read_profile() do
  global profile
  return profile
end
func prescan_score() do
  score += 2
  global score
  return score
end
func global_meta() do
  global banner
  global numbers
  global profile
  return type(banner) + ":" + to_str(length(numbers)) + ":" + to_str(to_bool(profile))
end
func init_value_globals() do
  global active_flag
  global missing_value
  global handler_state
  active_flag is true
  missing_value is nil
  handler_state is global_value_handler
end
func global_value_meta() do
  global active_flag
  global missing_value
  global handler_state
  return type(active_flag) + ":" + to_str(to_bool(active_flag)) + ":" + type(missing_value) + ":" + to_str(to_bool(missing_value)) + ":" + type(handler_state) + ":" + to_str(to_bool(handler_state)) + ":" + to_str(handler_state)
end
func init_state() do
  global state
  state is {score: 4, name: "Sura"}
end
func init_cond_state() do
  global cond_state
  cond_state is {score: 21, name: "Cond"}
  return true
end
func init_while_state() do
  global while_state
  while_state is {score: 22, name: "While"}
  return false
end
func init_repeat_state() do
  global repeat_state
  repeat_state is {score: 23, name: "Repeat"}
  return 0
end
func init_for_state() do
  global for_state
  for_state is {score: 24, name: "For"}
  return 2
end
func init_foreach_state() do
  global foreach_state
  foreach_state is {score: 25, name: "Foreach"}
  return []
end
func init_match_state() do
  global match_state
  match_state is {score: 26, name: "Match"}
  return 1
end
func init_throw_state() do
  global throw_state
  throw_state is {score: 27, name: "Throw"}
  return "boom"
end
func init_dot_assign_state() do
  global dot_assign_state
  dot_assign_state is {score: 28, name: "DotAssign"}
  return "dot"
end
func init_index_value_state() do
  global index_value_state
  index_value_state is {score: 29, name: "IndexValue"}
  return "idx"
end
func init_index_key_state() do
  global index_key_state
  index_key_state is {score: 30, name: "IndexKey"}
  return "dyn"
end
func init_index_read_key_state() do
  global index_read_key_state
  index_read_key_state is {score: 31, name: "IndexReadKey"}
  return "read"
end
func init_index_read_numeric_state() do
  global index_read_numeric_state
  index_read_numeric_state is {score: 32, name: "IndexReadNumeric"}
  return 1
end
func init_index_read_function_state() do
  global index_read_function_state
  index_read_function_state is {score: 33, name: "IndexReadFunction"}
  return "fn"
end
func init_index_to_str_state() do
  global index_to_str_state
  index_to_str_state is {score: 34, name: "IndexToString"}
  return 1
end
func init_index_type_state() do
  global index_type_state
  index_type_state is {score: 35, name: "IndexType"}
  return 1
end
func init_index_length_state() do
  global index_length_state
  index_length_state is {score: 36, name: "IndexLength"}
  return 1
end
func selected_index_read_function() do
  return 1
end
func init_function_return_stringify_state() do
  global function_return_stringify_state
  function_return_stringify_state is {score: 37, name: "FunctionReturnStringify"}
  return []
end
func init_function_arg_return_stringify_state(marker) do
  global function_arg_return_stringify_state
  function_arg_return_stringify_state is {score: 39, name: "FunctionArgReturnStringify"}
  return []
end
func make_array_return_stringify(marker) do
  global array_return_stringify_count
  array_return_stringify_count += 1
  return ["ArrayOnce", 41]
end
class GlobalSetup do
  func init() do
    global ctor_state
    ctor_state is {score: 9, name: "Ctor"}
  end
  func init_method_state() do
    global method_state
    method_state is {score: 7, name: "Method"}
  end
end
class ReturnStringifySetup do
  func init_method_return_stringify_state() do
    global method_return_stringify_state
    method_return_stringify_state is {score: 38, name: "MethodReturnStringify"}
    return {}
  end
  func init_method_arg_return_stringify_state(marker) do
    global method_arg_return_stringify_state
    method_arg_return_stringify_state is {score: 40, name: "MethodArgReturnStringify"}
    return {}
  end
  func make_dict_return_stringify(marker) do
    global dict_return_stringify_count
    dict_return_stringify_count += 1
    return {first: "DictOnce", second: "Done"}
  end
end
class ParentGlobalSetup do
  func init() do
    global parent_state
    parent_state is {score: 13, name: "Parent"}
  end
end
class ChildGlobalSetup extends ParentGlobalSetup do
  func init() do
    super.init()
  end
end
setup is new GlobalSetup()
child_setup is new ChildGlobalSetup()
return_stringify_setup is new ReturnStringifySetup()
add_score()
prescan_score()
init_value_globals()
init_state()
setup.init_method_state()
if init_cond_state() then
  print("ready")
end
while init_while_state() do
  print("skip while")
end
repeat init_repeat_state() do
  print("skip repeat")
end
for skipped in init_for_state() to 0 do
  print("skip for")
end
for skipped_item in init_foreach_state() do
  print("skip foreach")
end
match init_match_state()
when 1 then
  print("match ready")
when _ then
  print("match fallback")
end
try
  throw init_throw_state()
catch err
  print(err)
end
assignment_sink is {}
assignment_sink.dot is init_dot_assign_state()
assignment_sink["value"] is init_index_value_state()
assignment_sink[init_index_key_state()] is "keyed"
read_sink is {read: "ok"}
read_items is [7, "ok"]
read_functions is {fn: selected_index_read_function}
read_key_value is read_sink[init_index_read_key_state()]
read_numeric_value is read_items[init_index_read_numeric_state()]
read_function_value is to_str(read_functions[init_index_read_function_state()])
read_to_str_value is to_str(read_items[init_index_to_str_state()])
read_type_value is type(read_items[init_index_type_state()])
read_length_value is length(read_items[init_index_length_state()])
function_return_stringify_value is to_str(init_function_return_stringify_state())
method_return_stringify_value is to_str(return_stringify_setup.init_method_return_stringify_state())
function_arg_return_stringify_value is to_str(init_function_arg_return_stringify_state(1))
method_arg_return_stringify_value is to_str(return_stringify_setup.init_method_arg_return_stringify_state(1))
array_return_stringify_value is to_str(make_array_return_stringify(1))
dict_return_stringify_value is to_str(return_stringify_setup.make_dict_return_stringify(1))
print(global_value_meta())
print(to_str(read_score()) + read_banner() + to_str(read_numbers()[1]) + items[1] + to_str(read_profile().score) + to_str(profile.bonus) + global_meta() + type(state) + state.name + to_str(state.score) + type(method_state) + method_state.name + to_str(method_state.score) + type(ctor_state) + ctor_state.name + to_str(ctor_state.score) + type(parent_state) + parent_state.name + to_str(parent_state.score) + type(cond_state) + cond_state.name + to_str(cond_state.score) + type(while_state) + while_state.name + to_str(while_state.score) + type(repeat_state) + repeat_state.name + to_str(repeat_state.score) + type(for_state) + for_state.name + to_str(for_state.score) + type(foreach_state) + foreach_state.name + to_str(foreach_state.score) + type(match_state) + match_state.name + to_str(match_state.score) + type(throw_state) + throw_state.name + to_str(throw_state.score) + type(dot_assign_state) + dot_assign_state.name + to_str(dot_assign_state.score) + type(index_value_state) + index_value_state.name + to_str(index_value_state.score) + type(index_key_state) + index_key_state.name + to_str(index_key_state.score) + read_key_value + read_numeric_value + read_function_value + read_to_str_value + read_type_value + to_str(read_length_value) + function_return_stringify_value + method_return_stringify_value + function_arg_return_stringify_value + method_arg_return_stringify_value + array_return_stringify_value + dict_return_stringify_value + type(index_read_key_state) + index_read_key_state.name + to_str(index_read_key_state.score) + type(index_read_numeric_state) + index_read_numeric_state.name + to_str(index_read_numeric_state.score) + type(index_read_function_state) + index_read_function_state.name + to_str(index_read_function_state.score) + type(index_to_str_state) + index_to_str_state.name + to_str(index_to_str_state.score) + type(index_type_state) + index_type_state.name + to_str(index_type_state.score) + type(index_length_state) + index_length_state.name + to_str(index_length_state.score) + type(function_return_stringify_state) + function_return_stringify_state.name + to_str(function_return_stringify_state.score) + type(method_return_stringify_state) + method_return_stringify_state.name + to_str(method_return_stringify_state.score) + type(function_arg_return_stringify_state) + function_arg_return_stringify_state.name + to_str(function_arg_return_stringify_state.score) + type(method_arg_return_stringify_state) + method_arg_return_stringify_state.name + to_str(method_arg_return_stringify_state.score) + to_str(array_return_stringify_count) + to_str(dict_return_stringify_count))
'@ | Set-Content -LiteralPath $globalSource -Encoding UTF8
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $globalSource -Out $globalWat | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "WASM target should accept global declarations from AST JSON input"
    }
    $globalWatText = [System.IO.File]::ReadAllText($globalWat, [System.Text.Encoding]::ASCII)
    if ($globalWatText -notmatch ';; global score' -or
        $globalWatText -notmatch ';; global banner' -or
        $globalWatText -notmatch ';; global numbers' -or
        $globalWatText -notmatch ';; global items' -or
        $globalWatText -notmatch ';; global profile' -or
        $globalWatText -notmatch ';; global active_flag' -or
        $globalWatText -notmatch ';; global missing_value' -or
        $globalWatText -notmatch ';; global handler_state' -or
        $globalWatText -notmatch ';; global state' -or
        $globalWatText -notmatch ';; global method_state' -or
        $globalWatText -notmatch ';; global ctor_state' -or
        $globalWatText -notmatch ';; global parent_state' -or
        $globalWatText -notmatch ';; global cond_state' -or
        $globalWatText -notmatch ';; global while_state' -or
        $globalWatText -notmatch ';; global repeat_state' -or
        $globalWatText -notmatch ';; global for_state' -or
        $globalWatText -notmatch ';; global foreach_state' -or
        $globalWatText -notmatch ';; global match_state' -or
        $globalWatText -notmatch ';; global throw_state' -or
        $globalWatText -notmatch ';; global dot_assign_state' -or
        $globalWatText -notmatch ';; global index_value_state' -or
        $globalWatText -notmatch ';; global index_key_state' -or
        $globalWatText -notmatch ';; global index_read_key_state' -or
        $globalWatText -notmatch ';; global index_read_numeric_state' -or
        $globalWatText -notmatch ';; global index_read_function_state' -or
        $globalWatText -notmatch ';; global index_to_str_state' -or
        $globalWatText -notmatch ';; global index_type_state' -or
        $globalWatText -notmatch ';; global index_length_state' -or
        $globalWatText -notmatch ';; global function_return_stringify_state' -or
        $globalWatText -notmatch ';; global method_return_stringify_state' -or
        $globalWatText -notmatch ';; global function_arg_return_stringify_state' -or
        $globalWatText -notmatch ';; global method_arg_return_stringify_state' -or
        $globalWatText -notmatch ';; global array_return_stringify_count' -or
        $globalWatText -notmatch ';; global dict_return_stringify_count' -or
        $globalWatText -notmatch '\(global \$__sura_global_score \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_banner \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_numbers \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_items \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_profile \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_active_flag \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_missing_value \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_handler_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_method_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_ctor_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_parent_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_cond_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_while_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_repeat_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_for_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_foreach_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_match_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_throw_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_dot_assign_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_value_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_key_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_read_key_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_read_numeric_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_read_function_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_to_str_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_type_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_index_length_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_function_return_stringify_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_method_return_stringify_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_function_arg_return_stringify_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_method_arg_return_stringify_state \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_array_return_stringify_count \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch '\(global \$__sura_global_dict_return_stringify_count \(mut i32\) \(i32.const 0\)\)' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_score' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_score' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_banner' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_banner' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_numbers' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_numbers' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_items' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_items' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_profile' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_profile' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_active_flag' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_active_flag' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_missing_value' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_handler_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_handler_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_method_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_method_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_ctor_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_ctor_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_parent_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_parent_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_cond_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_cond_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_while_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_while_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_repeat_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_repeat_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_for_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_for_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_foreach_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_foreach_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_match_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_match_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_throw_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_throw_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_dot_assign_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_dot_assign_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_value_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_value_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_key_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_key_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_read_key_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_read_key_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_read_numeric_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_read_numeric_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_read_function_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_read_function_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_to_str_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_to_str_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_type_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_type_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_index_length_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_index_length_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_function_return_stringify_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_function_return_stringify_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_method_return_stringify_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_method_return_stringify_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_function_arg_return_stringify_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_function_arg_return_stringify_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_method_arg_return_stringify_state' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_method_arg_return_stringify_state' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_array_return_stringify_count' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_array_return_stringify_count' -or
        $globalWatText -notmatch 'global\.get \$__sura_global_dict_return_stringify_count' -or
        $globalWatText -notmatch 'global\.set \$__sura_global_dict_return_stringify_count' -or
        $globalWatText -notmatch 'call \$__sura_string_concat' -or
        $globalWatText -notmatch 'call \$__sura_dict_set' -or
        $globalWatText -notmatch 'i32\.store' -or
        $globalWatText -notmatch '\(func \$read_banner \(export "read_banner"\)' -or
        $globalWatText -notmatch '\(func \$read_numbers \(export "read_numbers"\)' -or
        $globalWatText -notmatch '\(func \$read_profile \(export "read_profile"\)' -or
        $globalWatText -notmatch '\(func \$prescan_score \(export "prescan_score"\)' -or
        $globalWatText -notmatch '\(func \$global_meta \(export "global_meta"\)' -or
        $globalWatText -notmatch '\(func \$global_value_handler \(export "global_value_handler"\)' -or
        $globalWatText -notmatch '\(func \$init_value_globals \(export "init_value_globals"\)' -or
        $globalWatText -notmatch '\(func \$global_value_meta \(export "global_value_meta"\)' -or
        $globalWatText -notmatch '\(func \$init_state \(export "init_state"\)' -or
        $globalWatText -notmatch '\(func \$init_cond_state \(export "init_cond_state"\)' -or
        $globalWatText -notmatch '\(func \$init_while_state \(export "init_while_state"\)' -or
        $globalWatText -notmatch '\(func \$init_repeat_state \(export "init_repeat_state"\)' -or
        $globalWatText -notmatch '\(func \$init_for_state \(export "init_for_state"\)' -or
        $globalWatText -notmatch '\(func \$init_foreach_state \(export "init_foreach_state"\)' -or
        $globalWatText -notmatch '\(func \$init_match_state \(export "init_match_state"\)' -or
        $globalWatText -notmatch '\(func \$init_throw_state \(export "init_throw_state"\)' -or
        $globalWatText -notmatch '\(func \$init_dot_assign_state \(export "init_dot_assign_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_value_state \(export "init_index_value_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_key_state \(export "init_index_key_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_read_key_state \(export "init_index_read_key_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_read_numeric_state \(export "init_index_read_numeric_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_read_function_state \(export "init_index_read_function_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_to_str_state \(export "init_index_to_str_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_type_state \(export "init_index_type_state"\)' -or
        $globalWatText -notmatch '\(func \$init_index_length_state \(export "init_index_length_state"\)' -or
        $globalWatText -notmatch '\(func \$selected_index_read_function \(export "selected_index_read_function"\)' -or
        $globalWatText -notmatch '\(func \$init_function_return_stringify_state \(export "init_function_return_stringify_state"\)' -or
        $globalWatText -notmatch '\(func \$__sura_method_ReturnStringifySetup_init_method_return_stringify_state \(export "__sura_method_ReturnStringifySetup_init_method_return_stringify_state"\)' -or
        $globalWatText -notmatch '\(func \$init_function_arg_return_stringify_state \(export "init_function_arg_return_stringify_state"\)' -or
        $globalWatText -notmatch '\(func \$__sura_method_ReturnStringifySetup_init_method_arg_return_stringify_state \(export "__sura_method_ReturnStringifySetup_init_method_arg_return_stringify_state"\)' -or
        $globalWatText -notmatch '\(func \$make_array_return_stringify \(export "make_array_return_stringify"\)' -or
        $globalWatText -notmatch '\(func \$__sura_method_ReturnStringifySetup_make_dict_return_stringify \(export "__sura_method_ReturnStringifySetup_make_dict_return_stringify"\)' -or
        $globalWatText -notmatch '\(func \$__sura_new_GlobalSetup \(export "__sura_new_GlobalSetup"\)' -or
        $globalWatText -notmatch '\(func \$__sura_new_ChildGlobalSetup \(export "__sura_new_ChildGlobalSetup"\)' -or
        $globalWatText -notmatch '\(func \$__sura_method_GlobalSetup_init_method_state \(export "__sura_method_GlobalSetup_init_method_state"\)' -or
        $globalWatText -notmatch '(?s)call \$read_banner\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+call \$__sura_string_concat' -or
        $globalWatText -notmatch '(?s)call \$read_numbers\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+i32\.const 1\s+call \$__sura_array_get_checked' -or
        $globalWatText -notmatch '(?s)global\.get \$__sura_global_items\s+i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $globalWatText -notmatch '(?s)call \$read_profile\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)global\.get \$__sura_global_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_to_string' -or
        $globalWatText -notmatch '(?s)\(func \$add_score.*?global\.get \$__sura_global_items\s+i32\.const 1\s+.*?call \$__sura_array_set_checked' -or
        $globalWatText -notmatch '(?s)\(func \$add_score.*?global\.get \$__sura_global_profile.*?i32\.const 12\s+call \$__sura_dict_put\s+global\.set \$__sura_global_profile' -or
        $globalWatText -notmatch '(?s)call \$init_state.*?global\.get \$__sura_global_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_state.*?global\.get \$__sura_global_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$__sura_new_GlobalSetup.*?global\.get \$__sura_global_ctor_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$__sura_new_GlobalSetup.*?global\.get \$__sura_global_ctor_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$__sura_new_ChildGlobalSetup.*?global\.get \$__sura_global_parent_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$__sura_new_ChildGlobalSetup.*?global\.get \$__sura_global_parent_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$__sura_method_GlobalSetup_init_method_state.*?global\.get \$__sura_global_method_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$__sura_method_GlobalSetup_init_method_state.*?global\.get \$__sura_global_method_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_cond_state.*?if.*?global\.get \$__sura_global_cond_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_cond_state.*?if.*?global\.get \$__sura_global_cond_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_while_state.*?global\.get \$__sura_global_while_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_while_state.*?global\.get \$__sura_global_while_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_repeat_state.*?global\.get \$__sura_global_repeat_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_repeat_state.*?global\.get \$__sura_global_repeat_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_for_state.*?global\.get \$__sura_global_for_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_for_state.*?global\.get \$__sura_global_for_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_foreach_state.*?global\.get \$__sura_global_foreach_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_foreach_state.*?global\.get \$__sura_global_foreach_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_match_state.*?global\.get \$__sura_global_match_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_match_state.*?global\.get \$__sura_global_match_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_throw_state.*?br \$__try_end\d+.*?global\.get \$__sura_global_throw_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_throw_state.*?br \$__try_end\d+.*?global\.get \$__sura_global_throw_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_dot_assign_state.*?call \$__sura_dict_put.*?global\.get \$__sura_global_dot_assign_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_dot_assign_state.*?call \$__sura_dict_put.*?global\.get \$__sura_global_dot_assign_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_index_value_state.*?call \$__sura_dict_put.*?global\.get \$__sura_global_index_value_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_value_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+call \$__sura_dict_put.*?global\.get \$__sura_global_index_value_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_index_key_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp.*?call \$__sura_dict_put.*?global\.get \$__sura_global_index_key_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_key_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp.*?call \$__sura_dict_put.*?global\.get \$__sura_global_index_key_state\s+i32\.const \d+\s+call \$__sura_dict_get' -or
        $globalWatText -notmatch '(?s)call \$init_index_read_key_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop\s+i32\.const \d+\s+call \$__sura_dict_get.*?global\.get \$__sura_global_index_read_key_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_read_numeric_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop\s+i32\.const 1\s+call \$__sura_array_get_checked.*?global\.get \$__sura_global_index_read_numeric_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_read_function_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop.*?global\.get \$__sura_global_index_read_function_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_to_str_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop\s+i32\.const 1\s+call \$__sura_array_get_checked.*?global\.get \$__sura_global_index_to_str_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_type_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop\s+i32\.const 1\s+call \$__sura_array_get_checked\s+drop.*?global\.get \$__sura_global_index_type_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_index_length_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop\s+i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_length.*?global\.get \$__sura_global_index_length_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_function_return_stringify_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_array.*?call \$__sura_value_to_string\s+local\.set \$function_return_stringify_value.*?global\.get \$__sura_global_function_return_stringify_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$__sura_method_ReturnStringifySetup_init_method_return_stringify_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_dict.*?call \$__sura_value_to_string\s+local\.set \$method_return_stringify_value.*?global\.get \$__sura_global_method_return_stringify_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$init_function_arg_return_stringify_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_array.*?call \$__sura_value_to_string\s+local\.set \$function_arg_return_stringify_value.*?global\.get \$__sura_global_function_arg_return_stringify_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$__sura_method_ReturnStringifySetup_init_method_arg_return_stringify_state\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_dict.*?call \$__sura_value_to_string\s+local\.set \$method_arg_return_stringify_value.*?global\.get \$__sura_global_method_arg_return_stringify_state.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)call \$make_array_return_stringify\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp.*?local\.get \$__sura_wasm_value_tmp.*?local\.get \$__sura_wasm_value_tmp' -or
        $globalWatText -notmatch '(?s)call \$__sura_method_ReturnStringifySetup_make_dict_return_stringify\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp.*?local\.get \$__sura_wasm_value_tmp.*?local\.get \$__sura_wasm_value_tmp' -or
        $globalWatText -notmatch '(?s)\(func \$prescan_score.*?global\.get \$__sura_global_score\s+i32\.const 2\s+i32\.add\s+global\.set \$__sura_global_score' -or
        $globalWatText -notmatch '(?s)\(func \$global_meta.*?global\.get \$__sura_global_banner\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)\(func \$global_meta.*?global\.get \$__sura_global_numbers.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $globalWatText -notmatch '(?s)\(func \$global_meta.*?global\.get \$__sura_global_profile.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $globalWatText -notmatch '(?s)\(func \$global_value_meta.*?global\.get \$__sura_global_active_flag\s+call \$__sura_value_bool\s+call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)\(func \$global_value_meta.*?global\.get \$__sura_global_active_flag\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy' -or
        $globalWatText -notmatch '(?s)\(func \$global_value_meta.*?call \$__sura_value_nil\s+call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)\(func \$global_value_meta.*?call \$__sura_value_nil\s+call \$__sura_value_is_truthy' -or
        $globalWatText -notmatch '(?s)\(func \$global_value_meta.*?global\.get \$__sura_global_handler_state.*?call \$__sura_value_function.*?call \$__sura_value_type_name' -or
        $globalWatText -notmatch '(?s)\(func \$global_value_meta.*?global\.get \$__sura_global_handler_state.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy' -or
        $globalWatText -notmatch '(?s)\(func \$init_value_globals.*?i32\.const 1\s+global\.set \$__sura_global_active_flag.*?i32\.const 0\s+global\.set \$__sura_global_missing_value.*?i32\.const \d+\s+global\.set \$__sura_global_handler_state' -or
        $globalWatText -match 'local\.set \$(banner|numbers|items|profile|active_flag|missing_value|handler_state|state|method_state|ctor_state|parent_state|cond_state|while_state|repeat_state|for_state|foreach_state|match_state|throw_state|dot_assign_state|index_value_state|index_key_state|index_read_key_state|index_read_numeric_state|index_read_function_state|index_to_str_state|index_type_state|index_length_state|function_return_stringify_state|method_return_stringify_state|function_arg_return_stringify_state|method_arg_return_stringify_state|array_return_stringify_count|dict_return_stringify_count)(?![A-Za-z0-9_])') {
        throw "generated WAT should lower global declarations to mutable module-global get/set for numeric and dynamic value handles"
    }

    $globalMainStart = $globalWatText.IndexOf('(func (export "main")')
    $globalMainWatText = if ($globalMainStart -ge 0) { $globalWatText.Substring($globalMainStart) } else { $globalWatText }
    $arrayStringifyCallCount = [regex]::Matches($globalMainWatText, 'call \$make_array_return_stringify\b').Count
    $dictStringifyCallCount = [regex]::Matches($globalMainWatText, 'call \$__sura_method_ReturnStringifySetup_make_dict_return_stringify\b').Count
    if ($arrayStringifyCallCount -ne 1 -or $dictStringifyCallCount -ne 1) {
        throw "effectful collection-return to_str(value) should evaluate each function/method once, got array=$arrayStringifyCallCount dict=$dictStringifyCallCount"
    }

    if ($watText -notmatch '\(func \(export "main"\) \(result i32\)' -or
        $watText -notmatch 'call \$sum_to' -or
        $watText -notmatch 'call \$square') {
        throw "generated WAT should include exported main and numeric function calls"
    }
    if ($watText -notmatch '(?s)local\.get \$dynamic_unary_handler\s+i32\.const 10\s+call \$__sura_value_num\s+call \$__sura_call_function_1') {
        throw "generated WAT should box an indirect function-value argument before tagged-Value dispatch"
    }
    if ($watText -notmatch '(?s)\(func \$__sura_func_expr_wrapper_\d+.*?call \$bump_param' -or
        $watText -match '(?s)\(func \$__sura_func_expr_wrapper_\d+.*?call \$handler') {
        throw "captured function aliases inside lifted WASM function expressions should resolve to the captured lifted target"
    }
    if ($watText -notmatch '(?s)\(func \$__sura_func_expr_outer_\d+.*?(call \$__sura_func_expr_inner_\d+|i32\.const 1\s+i32\.add)' -or
        $watText -match '(?s)\(func \$__sura_func_expr_outer_\d+.*?call \$alias') {
        throw "captured aliases to lifted inline functions should resolve to the lifted target inside nested WASM closures"
    }
    if ($watText -notmatch '(?s)\(func \$captured_runtime_function_ternary_ast.*?local\.get \$handler\s+i32\.const 5\s+call \$__sura_value_num\s+call \$__sura_call_function_1' -or
        $watText -match '(?s)\(func \$__sura_func_expr_wrapper_\d+.*?call \$handler') {
        throw "immediately-called inline function wrappers should inline in the current WASM scope so runtime function-value captures dispatch through the local function id"
    }
    if ($watText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_function_capture_source_[^\s)]*.*?local\.get \$value\s+call \$bump_param.*?return' -or
        $watText -notmatch 'local\.set \$returned_param_function_capture_source_label' -or
        $watText -match '(?s)\(func \$__sura_func_expr_returned_param_function_capture_source_[^\s)]*.*?call \$handler') {
        throw "source WASM fixture should specialize returned closures that capture function-valued call arguments"
    }
    if ($watText -notmatch '\(func \$__sura_new_WasmLookupFunctionHolder \(export "__sura_new_WasmLookupFunctionHolder"\)' -or
        $watText -notmatch '\(func \$__sura_method_WasmLookupFunctionHolder_dict_handler_profile \(export "__sura_method_WasmLookupFunctionHolder_dict_handler_profile"\)' -or
        $watText -notmatch '\(func \$__sura_method_WasmLookupFunctionHolder_array_handler_function \(export "__sura_method_WasmLookupFunctionHolder_array_handler_function"\)') {
        throw "source WASM fixture should lower lookup-selected function-returning class methods through generated class functions"
    }
    $sourceLookupDictMethodStart = $watText.IndexOf('(func $__sura_method_WasmLookupFunctionHolder_dict_handler_profile')
    $sourceLookupDictMethodNext = if ($sourceLookupDictMethodStart -ge 0) { $watText.IndexOf("  (func ", $sourceLookupDictMethodStart + 1) } else { -1 }
    if ($sourceLookupDictMethodNext -lt 0) { $sourceLookupDictMethodNext = $watText.Length }
    $sourceLookupDictMethodBody = if ($sourceLookupDictMethodStart -ge 0) { $watText.Substring($sourceLookupDictMethodStart, $sourceLookupDictMethodNext - $sourceLookupDictMethodStart) } else { "" }
    if ($sourceLookupDictMethodBody -notmatch 'call \$__sura_call_function_1' -or $sourceLookupDictMethodBody -match 'call \$handler') {
        throw "source WASM fixture should dispatch method-local dict lookup-selected dict-returning function values indirectly"
    }
    $sourceLookupArrayMethodStart = $watText.IndexOf('(func $__sura_method_WasmLookupFunctionHolder_array_handler_function')
    $sourceLookupArrayMethodNext = if ($sourceLookupArrayMethodStart -ge 0) { $watText.IndexOf("  (func ", $sourceLookupArrayMethodStart + 1) } else { -1 }
    if ($sourceLookupArrayMethodNext -lt 0) { $sourceLookupArrayMethodNext = $watText.Length }
    $sourceLookupArrayMethodBody = if ($sourceLookupArrayMethodStart -ge 0) { $watText.Substring($sourceLookupArrayMethodStart, $sourceLookupArrayMethodNext - $sourceLookupArrayMethodStart) } else { "" }
    if ($sourceLookupArrayMethodBody -notmatch 'call \$__sura_call_function_1' -or $sourceLookupArrayMethodBody -match 'call \$handler') {
        throw "source WASM fixture should dispatch method-local array lookup-selected function-returning function values indirectly"
    }
    $sourceLookupReturnCallIndex = $watText.IndexOf('local.set $lookup_function_holder_return_label')
    if ($sourceLookupReturnCallIndex -lt 0) {
        throw "source WASM fixture should include lookup_function_holder_return_label"
    }
    $sourceLookupReturnCallStart = [Math]::Max(0, $sourceLookupReturnCallIndex - 3600)
    $sourceLookupReturnCallWindow = $watText.Substring($sourceLookupReturnCallStart, $sourceLookupReturnCallIndex - $sourceLookupReturnCallStart)
    if ($sourceLookupReturnCallWindow -notmatch 'call \$__sura_call_function_2' -or
        $sourceLookupReturnCallWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "source WASM fixture should preserve method-returned function values for later invocation"
    }
    if ($watText -notmatch 'recursive import expansion before lowering' -or
        $watText -notmatch '\(func \$wasm_import_double \(export "wasm_import_double"\)' -or
        $watText -notmatch 'local\.set \$wasm_import_value' -or
        $watText -notmatch 'call \$wasm_import_double') {
        throw "generated WAT should recursively expand imported numeric Sura files before lowering"
    }
    if ($watText -notmatch 'Tagged dynamic Value ABI' -or
        $watText -notmatch '\(func \$__sura_value_pack' -or
        $watText -notmatch '\(func \$__sura_value_nil' -or
        $watText -notmatch '\(func \$__sura_value_num' -or
        $watText -notmatch '\(func \$__sura_value_num_f64' -or
        $watText -notmatch '\(func \$__sura_value_number_f64' -or
        $watText -notmatch '\(func \$__sura_value_bool' -or
        $watText -notmatch '\(func \$__sura_value_string' -or
        $watText -notmatch '\(func \$__sura_value_dynamic_array' -or
        $watText -notmatch '\(func \$__sura_value_dynamic_dict' -or
        $watText -notmatch '\(func \$__sura_value_function' -or
        $watText -notmatch '\(func \$__sura_value_tag' -or
        $watText -notmatch '\(func \$__sura_value_payload' -or
        $watText -notmatch '\(func \$__sura_value_type_name' -or
        $watText -notmatch '\(func \$__sura_value_length' -or
        $watText -notmatch '\(func \$__sura_value_to_string' -or
        $watText -notmatch '\(func \$__sura_value_to_repr' -or
        $watText -notmatch '\(func \$__sura_value_array_to_string' -or
        $watText -notmatch '\(func \$__sura_value_dict_to_string' -or
        $watText -notmatch '\(func \$__sura_value_add' -or
        $watText -notmatch '\(func \$__sura_value_require_num' -or
        $watText -notmatch '\(func \$__sura_value_as_i32' -or
        $watText -notmatch '\(func \$__sura_value_is_truthy' -or
        $watText -notmatch '\(func \$__sura_value_eq' -or
        $watText -notmatch '\(func \$__sura_value_runtime_selftest \(export "__sura_value_runtime_selftest"\)') {
        throw "generated WAT should include tagged dynamic Value ABI helpers for full WASM lowering work"
    }
    if ($watText -notmatch 'numeric array literals/indexing/len via linear memory' -or
        $watText -notmatch 'array\.len/array\.sum/array\.avg/array\.min/array\.max/array\.range/array\.index_of/array\.contains helpers' -or
        $watText -notmatch 'inline numeric array literals as call arguments' -or
        $watText -notmatch 'numeric array and indexed numeric array for-in loops' -or
        $watText -notmatch '\(memory \(export "memory"\) 1 1024\)' -or
        $watText -notmatch '\(func \$__sura_alloc' -or
        $watText -notmatch '\(func \$__sura_array_sum' -or
        $watText -notmatch '\(func \$__sura_array_avg' -or
        $watText -notmatch '\(func \$__sura_array_min' -or
        $watText -notmatch '\(func \$__sura_array_max' -or
        $watText -notmatch '\(func \$__sura_array_range' -or
        $watText -notmatch '\(func \$__sura_array_index_of' -or
        $watText -notmatch '\(func \$__sura_array_contains' -or
        $watText -notmatch 'call \$__sura_alloc' -or
        $watText -notmatch 'call \$__sura_array_sum' -or
        $watText -notmatch 'call \$__sura_(?:array_avg|value_array_avg)' -or
        $watText -notmatch 'call \$__sura_array_min' -or
        $watText -notmatch 'call \$__sura_array_max' -or
        $watText -notmatch 'call \$__sura_array_range' -or
        $watText -notmatch 'call \$__sura_array_index_of' -or
        $watText -notmatch 'call \$__sura_array_contains' -or
        $watText -notmatch 'call \$sum3' -or
        $watText -notmatch '\(func \$make_values \(export "make_values"\)' -or
        $watText -notmatch '\(func \$make_mixed_values \(export "make_mixed_values"\)' -or
        $watText -notmatch '\(func \$make_mixed_profile \(export "make_mixed_profile"\)' -or
        $watText -notmatch 'call \$make_values' -or
        $watText -notmatch 'call \$make_mixed_values' -or
        $watText -notmatch 'call \$make_mixed_profile' -or
        $watText -notmatch 'local\.set \$wasm_values' -or
        $watText -notmatch 'local\.set \$range_values' -or
        $watText -notmatch 'local\.set \$made_values' -or
        $watText -notmatch 'local\.set \$mixed_from_function' -or
        $watText -notmatch 'local\.set \$function_mixed_label' -or
        $watText -notmatch 'local\.set \$direct_function_mixed_label' -or
        $watText -notmatch 'local\.set \$direct_function_mixed_dict_label' -or
        $watText -notmatch 'local\.set \$direct_function_mixed_dot_label' -or
        $watText -notmatch '\(local \$__(?:ast_)?for_array' -or
        $watText -notmatch 'local\.set \$array_for_total' -or
        $watText -notmatch 'local\.set \$inline_array_for_total' -or
        $watText -notmatch 'local\.set \$array_indexed_total' -or
        $watText -notmatch 'local\.set \$inline_indexed_total' -or
        $watText -notmatch 'local\.set \$idx_item' -or
        $watText -notmatch 'local\.set \$indexed_item' -or
        $watText -notmatch 'local\.set \$inline_idx' -or
        $watText -notmatch 'local\.set \$inline_item' -or
        $watText -notmatch 'call \$__sura_make_array_\d+' -or
        $watText -notmatch 'i32\.store' -or
        $watText -notmatch 'i32\.load') {
        throw "generated WAT should lower numeric array literals, inline array literals, indexing, len(), array.range(), array for-in, indexed array for-in, and array arguments through linear memory"
    }
    if ($watText -match 'call \$(array\.|array_sum|array_len|array_avg|array_min|array_max|array_range)') {
        throw "generated WAT should lower array.len/array.sum/array.avg/array.min/array.max/array.range/array.index_of/array.contains aliases instead of leaving unresolved calls"
    }
    if ($watText -match 'call \$make_valuescall') {
        throw "generated WAT should keep nested function-call lowering on separate instructions"
    }
    if ($watText -notmatch 'integer 3D vector helpers' -or
        $watText -notmatch '\(func \$__sura_vec3_new' -or
        $watText -notmatch '\(func \$__sura_vec3_cross' -or
        $watText -notmatch 'call \$__sura_vec3_add' -or
        $watText -notmatch 'call \$__sura_vec3_sub' -or
        $watText -notmatch 'call \$__sura_vec3_dot' -or
        $watText -notmatch 'call \$__sura_vec3_cross' -or
        $watText -notmatch 'call \$__sura_vec3_scale' -or
        $watText -notmatch 'call \$__sura_vec3_norm' -or
        $watText -notmatch 'call \$__sura_vec3_distance' -or
        $watText -notmatch 'local\.set \$wasm_p3' -or
        $watText -notmatch 'local\.set \$wasm_q3' -or
        $watText -notmatch 'local\.set \$wasm_cross3') {
        throw "generated WAT should lower integer 3D vector helpers through linear-memory arrays"
    }
    if ($watText -match 'call \$(vec3|vector3|vector\.)') {
        throw "generated WAT should lower 3D vector aliases instead of leaving unresolved calls"
    }
    if ($watText -notmatch 'exported top-level numeric functions' -or
        $watText -notmatch '\(func \$square \(export "square"\)' -or
        $watText -notmatch '\(func \$sum_to \(export "sum_to"\)' -or
        $watText -notmatch '\(func \$fib \(export "fib"\)') {
        throw "generated WAT should export top-level numeric Sura functions for host calls"
    }
    if ($watText -notmatch '\(func \$fib' -or
        $watText -notmatch 'recursive numeric calls' -or
        $watText -notmatch 'inline if-then statements' -or
        ($watText | Select-String -Pattern 'call \$fib' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should lower recursive numeric functions with inline if-then returns"
    }
    if ($watText -notmatch '\(func \$bare_return_zero' -or
        $watText -notmatch 'i32\.const 0\s+return') {
        throw "generated WAT should lower bare numeric return statements to zero-valued returns"
    }
    if ($watText -notmatch '\(func \$return_direct_string \(export "return_direct_string"\)' -or
        $watText -notmatch '\(func \$return_direct_bool \(export "return_direct_bool"\)' -or
        $watText -notmatch '\(func \$return_direct_nil \(export "return_direct_nil"\)' -or
        $watText -notmatch '\(func \$return_direct_array \(export "return_direct_array"\)' -or
        $watText -notmatch '\(func \$return_direct_dict \(export "return_direct_dict"\)' -or
        $watText -notmatch '\(func \$return_direct_function \(export "return_direct_function"\)' -or
        $watText -notmatch '(?s)\(func \$return_direct_string.*?return' -or
        $watText -notmatch '(?s)\(func \$return_direct_bool.*?i32\.const 1\s+return' -or
        $watText -notmatch '(?s)\(func \$return_direct_nil.*?i32\.const 0\s+return' -or
        $watText -notmatch '(?s)\(func \$return_direct_array.*?call \$__sura_make_array_\d+\s+return' -or
        $watText -notmatch '(?s)\(func \$return_direct_dict.*?call \$__sura_make_dict_\d+\s+return' -or
        $watText -notmatch '(?s)\(func \$return_direct_function.*?i32\.const \d+\s+return' -or
        $watText -notmatch 'local\.set \$direct_return_runtime_label') {
        throw "generated WAT should lower direct dynamic return statements for string/bool/nil/array/dict/function values"
    }
    if ($watText -notmatch '\(param \$x i32\)' -or
        $watText -notmatch '\(param \$n i32\)' -or
        $watText -match '\(local \$x i32\)' -or
        $watText -match '\$x:\s*int' -or
        $watText -match '\$n:\s*int') {
        throw "generated WAT should strip Sura type annotations from numeric params"
    }
    if ($watText -notmatch 'is assignment/reassignment' -or
        $watText -notmatch 'call \$bump_param' -or
        ($watText | Select-String -Pattern 'local\.set \$assign_score' -AllMatches).Matches.Count -lt 2 -or
        ($watText | Select-String -Pattern 'local\.set \$x' -AllMatches).Matches.Count -lt 1) {
        throw "generated WAT should lower is assignment/reassignment without shadowing params"
    }
    if (($watText | Select-String -Pattern 'local\.set \$assign_string_value' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.set \$assign_bool_value' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.set \$assign_nil_value' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.set \$assign_array_value' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.set \$assign_dict_value' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.set \$assign_function_value' -AllMatches).Matches.Count -lt 1 -or
        $watText -notmatch 'call \$__sura_value_array' -or
        $watText -notmatch 'call \$__sura_value_dict' -or
        $watText -notmatch 'call \$__sura_value_function' -or
        $watText -notmatch 'call \$__sura_value_type_name' -or
        $watText -notmatch 'call \$__sura_value_to_string') {
        throw "generated WAT should lower dynamic string/bool/nil/array/dict/function assignments through local storage and Value helpers"
    }
    if ($watText -notmatch 'local\.set \$expr_stmt_value' -or
        ($watText | Select-String -Pattern '\bdrop\b' -AllMatches).Matches.Count -lt 1) {
        throw "generated WAT should lower numeric expression statements and drop their result"
    }
    if (($watText | Select-String -Pattern 'local\.get \$assign_string_value\s+drop' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.get \$assign_bool_value\s+drop' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.get \$assign_nil_value\s+drop' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.get \$assign_array_value\s+drop' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.get \$assign_dict_value\s+drop' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'local\.get \$assign_function_value\s+drop' -AllMatches).Matches.Count -lt 1 -or
        $watText -notmatch '(?s)local\.get \$assign_function_value\s+drop.*?local\.get \$assign_array_value.*?\bdrop\b.*?local\.get \$assign_dict_value.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy\s+drop') {
        throw "generated WAT should lower dynamic expression statements and drop string/bool/nil/array/dict/function results after evaluation"
    }
    if ($watText -notmatch 'ternary expressions' -or
        $watText -notmatch 'if \(result i32\)' -or
        $watText -notmatch 'local\.set \$ternary_score') {
        throw "generated WAT should lower numeric ternary expressions"
    }
    if ($watText -match 'call \$assert' -or $watText -match 'call \$assert_eq' -or $watText -match 'call \$assert_ne' -or $watText -match 'call \$assert_type' -or $watText -match 'call \$assert_len') {
        throw "assert helpers should lower to WAT checks, not unresolved function calls"
    }
    if ($watText -notmatch 'space-form assertions') {
        throw "generated WAT should document and lower space-form assertion statements"
    }
    if ($watText -notmatch 'side-effect-preserving throw expression evaluation before traps' -or
        $watText -notmatch 'local\.set \$throw_guard' -or
        $watText -notmatch '\(func \$throw_value_source \(export "throw_value_source"\)' -or
        $watText -notmatch '(?s)call \$throw_value_source\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?unreachable.*?end\s+local\.get \$__sura_wasm_call_tmp\s+call \$__sura_value_num\s+drop\s+unreachable' -or
        ($watText | Select-String -Pattern '\bunreachable\b' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should evaluate throw expressions for side effects before trapping"
    }
    if ($watText -match 'call \$(print|print_n)' -or
        $watText -notmatch 'print/print_n/print_no_nl/print\(\)-as-main-result with multi-argument dynamic print stringification' -or
        ($watText | Select-String -Pattern 'local\.set \$__result' -AllMatches).Matches.Count -lt 8) {
        throw "generated WAT should lower print/print_n/print_no_nl calls to the main result slot"
    }
    if ($watText -notmatch 'call \$__sura_value_dynamic_array' -or
        $watText -notmatch 'call \$__sura_value_dynamic_dict' -or
        $watText -notmatch '(?s)i32\.const 82.*?call \$__sura_make_array_6\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_to_string.*?local\.get \$result\s+call \$__sura_value_to_string.*?call \$__sura_value_dynamic_array\s+call \$__sura_value_to_string.*?call \$__sura_value_dynamic_dict\s+call \$__sura_value_to_string') {
        throw "generated WAT should lower multi-argument dynamic print through shared stringification helpers"
    }
    if ($watText -notmatch 'local\.set \$array_literal_runtime_label' -or
        $watText -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_type_name.*?call \$__sura_value_dynamic_array\s+call \$__sura_value_length.*?call \$__sura_value_dynamic_array\s+call \$__sura_value_is_truthy.*?call \$__sura_value_dynamic_array\s+call \$__sura_value_is_truthy.*?call \$__sura_value_dynamic_array\s+call \$__sura_value_to_string.*?local\.set \$array_literal_runtime_label') {
        throw "generated WAT should lower array literals directly through dynamic Value type/length/truthiness/stringification helpers"
    }
    if ($watText -notmatch 'local\.set \$dict_literal_runtime_label' -or
        $watText -notmatch '(?s)local\.get \$dict_literal_runtime_value.*?call \$__sura_value_dict.*?call \$__sura_value_type_name.*?local\.get \$dict_literal_runtime_value.*?call \$__sura_value_dict.*?call \$__sura_value_length.*?local\.get \$dict_literal_truthy_value.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy.*?local\.get \$dict_literal_empty_value.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $watText -notmatch '(?s)local\.get \$dict_literal_nested_value\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get.*?local\.get \$dict_literal_nested_value\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_array_to_string_string.*?local\.set \$dict_literal_runtime_label') {
        throw "generated WAT should lower dict literal type/length/truthiness dynamically and nested collection stringification with ordered entry types"
    }
    if ($watText -match 'call \$console' -or
        $watText -match 'local\.get \$console' -or
        $watText -notmatch 'console output API as main-result text') {
        throw "generated WAT should lower console output API calls to the main result text slot"
    }
    if ($watText -notmatch 'unreachable') {
        throw "generated WAT should include unreachable for lowered assertions"
    }
    if ($watText -notmatch 'i32\.and' -or
        $watText -notmatch 'i32\.or' -or
        $watText -notmatch 'i32\.eqz') {
        throw "generated WAT should lower numeric and/or/not expressions"
    }
    if ($watText -notmatch 'local\.set \$runtime_type_label' -or
        $watText -notmatch '(?s)local\.get \$result\s+call \$__sura_value_type_name' -or
        $watText -notmatch '(?s)i32\.const 1\s+call \$__sura_value_bool\s+call \$__sura_value_type_name' -or
        $watText -notmatch '(?s)local\.get \$runtime_type_profile.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $watText -notmatch '(?s)local\.get \$wasm_values.*?call \$__sura_value_array.*?call \$__sura_value_type_name') {
        throw "generated WAT should route primitive and collection type() expressions through the Value type helper"
    }
    if ($watText -notmatch 'local\.set \$runtime_length_label' -or
        $watText -notmatch '(?s)local\.get \$runtime_type_profile.*?call \$__sura_value_dict.*?call \$__sura_value_length' -or
        $watText -notmatch '(?s)local\.get \$wasm_values.*?call \$__sura_value_array.*?call \$__sura_value_length') {
        throw "generated WAT should route collection length() expressions through the Value length helper"
    }
    if ($watText -notmatch 'local\.set \$runtime_to_str_label' -or
        $watText -notmatch '(?s)i32\.const 32\s+call \$__sura_value_num\s+call \$__sura_value_to_string' -or
        $watText -notmatch '(?s)i32\.const 1\s+call \$__sura_value_bool\s+call \$__sura_value_to_string' -or
        $watText -notmatch '(?s)call \$__sura_value_nil\s+call \$__sura_value_to_string' -or
        $watText -notmatch '(?s)local\.get \$wasm_string\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_to_string') {
        throw "generated WAT should route primitive to_str() expressions through the Value string helper"
    }
    if ($watText -notmatch 'local\.set \$nested_collection_to_str_label' -or
        $watText -notmatch '(?s)local\.get \$wasm_names_to_str\s+call \$__sura_array_to_string_string.*?local\.get \$wasm_profile_to_str\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_string_concat.*?local\.set \$nested_collection_to_str_label') {
        throw "generated WAT should stringify array/dict variables nested inside to_str() literals"
    }
    if ($watText -notmatch 'local\.set \$runtime_collection_to_str_label' -or
        $watText -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_to_string.*?call \$__sura_value_dynamic_dict\s+call \$__sura_value_to_string.*?local\.set \$runtime_collection_to_str_label') {
        throw "generated WAT should stringify primitive array/dict literals through dynamic Value collection helpers"
    }
    if ($watText -notmatch 'bitwise/shift expressions' -or
        $watText -notmatch 'unary bitwise-not' -or
        $watText -notmatch 'nil sentinel literal' -or
        $watText -notmatch 'string literal array-like lowering' -or
        $watText -notmatch 'string indexing via __sura_string_at' -or
        $watText -notmatch 'String helpers' -or
        $watText -notmatch '\(func \$__sura_string_at' -or
        $watText -notmatch '\(func \$__sura_string_contains' -or
        $watText -notmatch '\(func \$__sura_string_index_of' -or
        $watText -notmatch '\(func \$__sura_string_starts_with' -or
        $watText -notmatch '\(func \$__sura_string_ends_with' -or
        $watText -notmatch '\(func \$__sura_string_upper' -or
        $watText -notmatch '\(func \$__sura_string_lower' -or
        $watText -notmatch '\(func \$__sura_string_trim' -or
        $watText -notmatch '\(func \$__sura_string_replace' -or
        $watText -notmatch '\(func \$__sura_string_substring' -or
        $watText -notmatch '\(func \$__sura_string_slice' -or
        $watText -notmatch 'call \$__sura_string_at' -or
        $watText -notmatch 'call \$__sura_string_contains' -or
        $watText -notmatch 'call \$__sura_string_index_of' -or
        $watText -notmatch 'call \$__sura_string_starts_with' -or
        $watText -notmatch 'call \$__sura_string_ends_with' -or
        $watText -notmatch 'call \$__sura_string_upper' -or
        $watText -notmatch 'call \$__sura_string_lower' -or
        $watText -notmatch 'call \$__sura_string_trim' -or
        $watText -notmatch 'call \$__sura_string_replace' -or
        $watText -notmatch 'call \$__sura_string_substring' -or
        $watText -notmatch 'call \$__sura_string_slice' -or
        $watText -notmatch '\(func \$__sura_value_string_or_nil' -or
        $watText -notmatch 'call \$__sura_value_type_name' -or
        $watText -notmatch 'call \$__sura_value_length' -or
        $watText -notmatch 'call \$__sura_value_to_string' -or
        $watText -notmatch 'local\.set \$__sura_wasm_value_tmp' -or
        $watText -match 'local\.get \$nil(?![A-Za-z0-9_])' -or
        $watText -notmatch 'local\.set \$wasm_string' -or
        $watText -notmatch 'local\.set \$string_direct_length_label' -or
        $watText -notmatch 'local\.set \$string_index_label' -or
        $watText -notmatch 'local\.set \$string_dynamic_label' -or
        $watText -notmatch 'local\.set \$string_truth_label' -or
        $watText -notmatch 'local\.set \$string_search_label' -or
        $watText -notmatch 'local\.set \$string_camel_alias_label' -or
        $watText -notmatch 'local\.set \$string_direct_alias_label' -or
        $watText -notmatch 'local\.set \$string_index_of_label' -or
        $watText -notmatch 'local\.set \$string_direct_transform_label' -or
        $watText -notmatch 'local\.set \$string_transform_label' -or
        $watText -notmatch 'local\.set \$string_replace_label' -or
        $watText -notmatch 'local\.set \$string_direct_replace_slice_label' -or
        $watText -notmatch 'local\.set \$string_slice_label' -or
        $watText -notmatch 'local\.set \$truth_branch' -or
        $watText -notmatch 'call \$__sura_value_is_truthy' -or
        $watText -notmatch 'i32\.const 115' -or
        $watText -notmatch 'i32\.xor' -or
        $watText -notmatch 'i32\.const -1' -or
        $watText -notmatch 'i32\.shl' -or
        $watText -notmatch 'i32\.shr_s' -or
        $watText -notmatch 'local\.set \$bit_score' -or
        $watText -notmatch 'local\.set \$shift_score' -or
        $watText -notmatch 'local\.set \$not_score' -or
        $watText -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_is_truthy\s+i32\.eqz\s+local\.set \$unary_array_not' -or
        $watText -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_is_truthy\s+i32\.eqz\s+local\.set \$unary_empty_array_not' -or
        $watText -notmatch '(?s)call \$__sura_value_dynamic_dict\s+call \$__sura_value_is_truthy\s+i32\.eqz\s+local\.set \$unary_dict_not' -or
        $watText -notmatch '(?s)call \$__sura_value_dynamic_dict\s+call \$__sura_value_is_truthy\s+i32\.eqz\s+local\.set \$unary_empty_dict_not' -or
        $watText -notmatch '(?s)i32\.const \d+.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy\s+i32\.eqz\s+local\.set \$unary_function_not') {
        throw "generated WAT should lower all Sura unary operators, including Value truthiness for not"
    }
    if ($watText -match 'call \$string_contains' -or
        $watText -match 'call \$string_len' -or
        $watText -match 'call \$string_length' -or
        $watText -match 'call \$string_upper' -or
        $watText -match 'call \$string_lower' -or
        $watText -match 'call \$string_trim' -or
        $watText -match 'call \$string_replace' -or
        $watText -match 'call \$string_substring' -or
        $watText -match 'call \$string_slice' -or
        $watText -match 'call \$string_sub' -or
        $watText -match 'call \$string_indexOf' -or
        $watText -match 'call \$string_index_of' -or
        $watText -match 'call \$string_startsWith' -or
        $watText -match 'call \$string_starts_with' -or
        $watText -match 'call \$string_endsWith' -or
        $watText -match 'call \$string_ends_with') {
        throw "generated WAT should lower source direct string aliases without unresolved calls"
    }
    if ($watText -match 'call \$math\.' -or
        $watText -match 'call \$(abs|min|max|clamp|sign|pow|sqrt)' -or
        $watText -notmatch 'select' -or
        $watText -notmatch 'i32\.const 3') {
        throw "generated WAT should lower integer-safe math module intrinsics"
    }
    if ($watText -notmatch 'i32\.gt_s' -or
        $watText -notmatch 'i32\.lt_s' -or
        $watText -notmatch 'i32\.sub') {
        throw "generated WAT should lower sign/min/max-style numeric intrinsics"
    }
    if ($watText -notmatch 'variadic min/max' -or
        ($watText | Select-String -Pattern '\bselect\b' -AllMatches).Matches.Count -lt 8) {
        throw "generated WAT should fold variadic min/max numeric intrinsics"
    }
    if ($watText -notmatch 'decimal floor/ceil/round literal folding' -or
        $watText -match '3\.9|3\.1|3\.5|2\.9' -or
        ($watText | Select-String -Pattern 'i32\.const 4' -AllMatches).Matches.Count -lt 2 -or
        ($watText | Select-String -Pattern 'i32\.const 3' -AllMatches).Matches.Count -lt 2 -or
        ($watText | Select-String -Pattern 'i32\.const 2' -AllMatches).Matches.Count -lt 1) {
        throw "generated WAT should fold decimal floor/ceil/round literal calls"
    }
    if ($watText -notmatch 'signed round-half-away-from-zero' -or
        $watText -match '-3\.4|-3\.5|-3\.6|-3\.1|-3\.9' -or
        ($watText | Select-String -Pattern 'i32\.const -4' -AllMatches).Matches.Count -lt 3 -or
        ($watText | Select-String -Pattern 'i32\.const -3' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should fold signed decimal floor/ceil/round literals"
    }
    if ($watText -notmatch 'decimal comparison literal folding' -or
        $watText -match '3\.14|3\.15' -or
        ($watText | Select-String -Pattern 'i32\.const 1' -AllMatches).Matches.Count -lt 5) {
        throw "generated WAT should fold decimal literal comparisons"
    }
    if ($watText -match 'call \$(to_int|to_float|to_bool)' -or
        $watText -notmatch 'numeric conversion aliases to_int/to_float/to_bool') {
        throw "generated WAT should lower numeric to_int/to_float/to_bool conversion aliases without unresolved calls"
    }
    if ($watText -notmatch '\(func \$__sura_pow_i32' -or
        $watText -notmatch 'call \$__sura_pow_i32' -or
        $watText -notmatch 'br \$__pow_loop' -or
        $watText -notmatch 'integer pow/math\.pow') {
        throw "generated WAT should lower pow/math.pow through the integer pow helper"
    }
    if ($watText -notmatch '\(func \$__sura_sqrt_i32' -or
        $watText -notmatch 'call \$__sura_sqrt_i32' -or
        $watText -notmatch 'br \$__sqrt_loop' -or
        $watText -notmatch 'sqrt/math\.sqrt') {
        throw "generated WAT should lower sqrt/math.sqrt through the integer sqrt helper"
    }
    if (($watText | Select-String -Pattern '\belse\b' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should include nested else branches for elif lowering"
    }
    if ($watText -notmatch 'inline elif-then statements' -or
        $watText -notmatch 'local\.set \$inline_elif') {
        throw "generated WAT should lower inline elif branches"
    }
    if ($watText -notmatch 'inline else-then statements' -or
        $watText -notmatch 'local\.set \$inline_else') {
        throw "generated WAT should lower inline else branches"
    }
    if ($watText -notmatch 'inline branch-local declarations' -or
        $watText -notmatch '\(local \$inline_if_local i32\)' -or
        $watText -notmatch '\(local \$inline_else_local i32\)' -or
        $watText -notmatch '\(local \$inline_elif_local i32\)' -or
        $watText -notmatch '\(local \$inline_when_local i32\)' -or
        $watText -notmatch '\(local \$inline_match_local i32\)' -or
        $watText -notmatch 'local\.set \$inline_if_local' -or
        $watText -notmatch 'local\.set \$inline_else_local' -or
        $watText -notmatch 'local\.set \$inline_elif_local' -or
        $watText -notmatch 'local\.set \$inline_when_local' -or
        $watText -notmatch 'local\.set \$inline_match_local') {
        throw "generated WAT should collect locals declared inside inline branch arms"
    }
    if ($watText -notmatch 'block \$__break' -or
        $watText -notmatch 'loop \$__loop' -or
        $watText -notmatch 'block \$__continue' -or
        $watText -notmatch 'br \$__break' -or
        $watText -notmatch 'br \$__continue') {
        throw "generated WAT should lower break/continue with stable named loop labels"
    }
    if ($watText -notmatch 'fixed-count repeat' -or
        $watText -notmatch '\(local \$__(?:ast_)?repeat_limit' -or
        $watText -notmatch 'local\.set \$__(?:ast_)?repeat_limit' -or
        $watText -notmatch 'local\.get \$__(?:ast_)?repeat_limit' -or
        $watText -notmatch 'local\.set \$repeat_fixed_count') {
        throw "generated WAT should snapshot repeat count expressions before looping"
    }
    $repeatRuntimeIndex = $watText.LastIndexOf('local.set $repeat_runtime_label')
    $repeatRuntimeStart = $watText.IndexOf('local.set $repeat_dynamic_counts')
    if ($repeatRuntimeIndex -lt 0 -or $repeatRuntimeStart -lt 0 -or $repeatRuntimeStart -ge $repeatRuntimeIndex) {
        throw "generated WAT should include repeat_runtime_label assignment"
    }
    $repeatRuntimeWindow = $watText.Substring($repeatRuntimeStart, $repeatRuntimeIndex - $repeatRuntimeStart)
    $guardedReturnFunctionTypePattern = '(?s)call \$return_direct_function\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+call \$__sura_value_type_name'
    $guardedReturnFunctionTruthyPattern = '(?s)call \$return_direct_function\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+call \$__sura_value_is_truthy'
    $guardedReturnFunctionDropPattern = '(?s)call \$return_direct_function\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop'
    if ($repeatRuntimeWindow -notmatch '(?s)local\.get \$repeat_dynamic_counts\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$__(?:ast_)?repeat_limit\d+' -or
        $repeatRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $repeatRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $repeatRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $repeatRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $repeatRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $repeatRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower repeat count expressions and dynamic Value operations inside repeat bodies"
    }
    $forRuntimeIndex = $watText.LastIndexOf('local.set $for_runtime_label')
    $forRuntimeStart = $watText.IndexOf('local.set $for_dynamic_froms')
    if ($forRuntimeIndex -lt 0 -or $forRuntimeStart -lt 0 -or $forRuntimeStart -ge $forRuntimeIndex) {
        throw "generated WAT should include for_runtime_label assignment"
    }
    $forRuntimeWindow = $watText.Substring($forRuntimeStart, $forRuntimeIndex - $forRuntimeStart)
    if ($forRuntimeWindow -notmatch '(?s)local\.get \$for_dynamic_froms\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$for_runtime_i' -or
        $forRuntimeWindow -notmatch '(?s)local\.get \$for_dynamic_tos\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$__(?:ast_)?for_end\d+' -or
        $forRuntimeWindow -notmatch '(?s)local\.get \$for_dynamic_steps\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$__(?:ast_)?for_step\d+' -or
        $forRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $forRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $forRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $forRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $forRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $forRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower dynamic range-for bounds/step expressions and dynamic Value operations inside for bodies"
    }
    $whileRuntimeIndex = $watText.LastIndexOf('local.set $while_runtime_label')
    $whileRuntimeStart = $watText.IndexOf('local.set $while_dynamic_limits')
    if ($whileRuntimeIndex -lt 0 -or $whileRuntimeStart -lt 0 -or $whileRuntimeStart -ge $whileRuntimeIndex) {
        throw "generated WAT should include while_runtime_label assignment"
    }
    $whileRuntimeWindow = $watText.Substring($whileRuntimeStart, $whileRuntimeIndex - $whileRuntimeStart)
    if ($whileRuntimeWindow -notmatch '(?s)local\.get \$while_runtime_counter\s+local\.get \$while_dynamic_limits\s+i32\.const 0\s+call \$__sura_array_get_checked\s+i32\.lt_s' -or
        $whileRuntimeWindow -notmatch 'local\.set \$while_runtime_counter' -or
        $whileRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $whileRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $whileRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $whileRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $whileRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $whileRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower dynamic while conditions and dynamic Value operations inside while bodies"
    }
    $ifRuntimeIndex = $watText.LastIndexOf('local.set $if_runtime_label')
    $ifRuntimeStart = $watText.IndexOf('local.set $if_runtime_label')
    if ($ifRuntimeIndex -lt 0 -or $ifRuntimeStart -lt 0 -or $ifRuntimeStart -ge $ifRuntimeIndex) {
        throw "generated WAT should include if_runtime_label branch assignment"
    }
    $ifRuntimeWindow = $watText.Substring($ifRuntimeStart, $ifRuntimeIndex - $ifRuntimeStart)
    if ($ifRuntimeWindow -notmatch '(?s)call \$make_mixed_profile\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_is_truthy\s+if' -or
        $ifRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $ifRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $ifRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $ifRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $ifRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $ifRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower dynamic Value if conditions and dynamic Value operations inside then branches"
    }
    $ifElseRuntimeIndex = $watText.LastIndexOf('local.set $if_else_runtime_label')
    $ifElseRuntimeStart = $watText.IndexOf('local.set $if_else_runtime_label')
    if ($ifElseRuntimeIndex -lt 0 -or $ifElseRuntimeStart -lt 0 -or $ifElseRuntimeStart -ge $ifElseRuntimeIndex) {
        throw "generated WAT should include if_else_runtime_label branch assignment"
    }
    $ifElseRuntimeWindow = $watText.Substring($ifElseRuntimeStart, $ifElseRuntimeIndex - $ifElseRuntimeStart)
    if ($ifElseRuntimeWindow -notmatch '(?s)call \$make_mixed_profile\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_is_truthy\s+if' -or
        $ifElseRuntimeWindow -notmatch 'else' -or
        $ifElseRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $ifElseRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $ifElseRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $ifElseRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $ifElseRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $ifElseRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower dynamic Value if conditions and dynamic Value operations inside else branches"
    }
    foreach ($loopConflictName in @(
        "repeat_conflict_after", "repeat_conflict_field_after",
        "while_conflict_after", "while_conflict_field_after",
        "for_conflict_after", "for_conflict_field_after",
        "foreach_array_conflict_after", "foreach_array_conflict_field_after",
        "foreach_dict_conflict_after", "foreach_dict_conflict_field_after"
    )) {
        $afterIndex = $watText.IndexOf("local.set `$$loopConflictName")
        if ($afterIndex -lt 0) {
            throw "generated WAT should include $loopConflictName assignment"
        }
        $windowStart = [Math]::Max(0, $afterIndex - 320)
        $window = $watText.Substring($windowStart, $afterIndex - $windowStart)
        if ($window -match 'call \$__sura_string_concat' -or $window -notmatch 'i32\.add') {
            throw "generated WAT should merge loop body hints with the pre-loop state before $loopConflictName"
        }
    }
    $foreachValueIndex = $watText.IndexOf('local.set $foreach_value_summary')
    if ($foreachValueIndex -lt 0) {
        throw "generated WAT should include foreach_value_summary assignment"
    }
    $foreachValueStart = [Math]::Max(0, $foreachValueIndex - 80000)
    $foreachValueWindow = $watText.Substring($foreachValueStart, $foreachValueIndex - $foreachValueStart)
    if ($foreachValueWindow -notmatch 'local\.set \$foreach_string_value' -or
        $foreachValueWindow -notmatch 'local\.set \$foreach_bool_value' -or
        $foreachValueWindow -notmatch 'local\.set \$foreach_nil_value' -or
        $foreachValueWindow -notmatch 'local\.set \$foreach_array_value' -or
        $foreachValueWindow -notmatch 'local\.set \$foreach_dict_value' -or
        $foreachValueWindow -notmatch 'local\.set \$foreach_function_value' -or
        $foreachValueWindow -notmatch '(?s)call \$__sura_value_array.*?call \$__sura_value_length' -or
        $foreachValueWindow -notmatch '(?s)call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $foreachValueWindow -notmatch '(?s)call \$__sura_value_function.*?call \$__sura_value_type_name' -or
        $foreachValueWindow -notmatch '(?s)call \$__sura_value_function.*?call \$__sura_value_is_truthy') {
        throw "generated WAT should lower source foreach loop values for string/bool/nil/array/dict/function typed bodies"
    }
    if ($watText -notmatch 'local\.set \$when_pick' -or
        $watText -notmatch 'i32\.ge_s' -or
        $watText -notmatch 'i32\.le_s' -or
        $watText -notmatch 'when/is/in/else') {
        throw "generated WAT should lower Sura when/is/in/else numeric arms"
    }
    if ($watText -notmatch '\(local \$__ast_match_matched' -or
        $watText -notmatch 'local\.set \$__ast_match_matched' -or
        $watText -notmatch 'match/when wildcard arms' -or
        $watText -notmatch 'local\.set \$match_pick' -or
        $watText -notmatch 'local\.set \$match_wildcard') {
        throw "generated WAT should lower Sura match/when wildcard arms"
    }
    if ($watText -notmatch 'local\.set \$match_block_pick' -or
        $watText -notmatch 'i32\.eq') {
        throw "generated WAT should lower block-style match arms without then"
    }
    if ($watText -notmatch 'local\.set \$match_first_wildcard' -or
        $watText -notmatch 'i32\.const 1') {
        throw "generated WAT should lower first-wildcard match arms"
    }
    if ($watText -notmatch 'local\.set \$match_middle_wildcard' -or
        $watText -notmatch 'local\.set \$match_matched_before_wildcard') {
        throw "generated WAT should lower nonterminal wildcard match arms with native matched-arm semantics"
    }
    $matchRuntimeIndex = $watText.LastIndexOf('local.set $match_runtime_label')
    $matchRuntimeStart = $watText.IndexOf('local.set $match_runtime_label')
    if ($matchRuntimeIndex -lt 0 -or $matchRuntimeStart -lt 0 -or $matchRuntimeStart -ge $matchRuntimeIndex) {
        throw "generated WAT should include match_runtime_label arm assignment"
    }
    $matchRuntimeWindow = $watText.Substring($matchRuntimeStart, $matchRuntimeIndex - $matchRuntimeStart)
    if ($matchRuntimeWindow -notmatch '(?s)call \$make_mixed_profile\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_eq' -or
        $matchRuntimeWindow -notmatch 'local\.set \$__ast_match_matched\d+' -or
        $matchRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $matchRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $matchRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $matchRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $matchRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $matchRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower dynamic Value match subjects and dynamic Value operations inside match arms"
    }
    $matchNilRuntimeIndex = $watText.LastIndexOf('local.set $match_nil_runtime_label')
    $matchNilRuntimeStart = $watText.IndexOf('local.set $match_nil_runtime_label')
    if ($matchNilRuntimeIndex -lt 0 -or $matchNilRuntimeStart -lt 0 -or $matchNilRuntimeStart -ge $matchNilRuntimeIndex) {
        throw "generated WAT should include match_nil_runtime_label arm assignment"
    }
    $matchNilRuntimeWindow = $watText.Substring($matchNilRuntimeStart, $matchNilRuntimeIndex - $matchNilRuntimeStart)
    if ($matchNilRuntimeWindow -notmatch '(?s)call \$make_mixed_profile\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_eq' -or
        $matchNilRuntimeWindow -notmatch 'call \$__sura_value_nil' -or
        $matchNilRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $matchNilRuntimeWindow -notmatch '(?s)local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $matchNilRuntimeWindow -notmatch '(?s)local\.get \$runtime_truthy_dict.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $matchNilRuntimeWindow -notmatch $guardedReturnFunctionTypePattern -or
        $matchNilRuntimeWindow -notmatch $guardedReturnFunctionTruthyPattern -or
        $matchNilRuntimeWindow -notmatch $guardedReturnFunctionDropPattern) {
        throw "generated WAT should lower nil Value match subjects and dynamic Value operations inside match arms"
    }
    $sourceCompoundIndex = $watText.IndexOf('local.set $wasm_compound_ops_label')
    if ($sourceCompoundIndex -lt 0) {
        throw "generated WAT should include source compound operator label assignment"
    }
    $sourceCompoundStart = [Math]::Max(0, $sourceCompoundIndex - 12000)
    $sourceCompoundWindow = $watText.Substring($sourceCompoundStart, $sourceCompoundIndex - $sourceCompoundStart)
    if ($sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_score\s+i32\.const 4\s+i32\.sub\s+local\.set \$wasm_compound_score' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_score\s+i32\.const 3\s+i32\.mul\s+local\.set \$wasm_compound_score' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_score\s+call \$__sura_value_num\s+local\.set \$__sura_wasm_value_tmp\s+local\.get \$__sura_wasm_value_tmp\s+local\.set \$wasm_compound_score.*?local\.get \$__sura_wasm_value_tmp\s+i32\.const 2\s+call \$__sura_value_num\s+call \$__sura_value_div.*?local\.set \$wasm_compound_score' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_score\s+i32\.const 10\s+call \$__sura_value_num\s+call \$__sura_value_mod.*?local\.set \$wasm_compound_score' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_local_text.*?call \$__sura_string_concat\s+local\.set \$wasm_compound_local_text' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_numbers\s+i32\.const 1\s+local\.get \$wasm_compound_numbers\s+i32\.const 1\s+call \$__sura_array_get_checked\s+i32\.const 4\s+i32\.add\s+call \$__sura_array_set_checked' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_num_profile\s+i32\.const \d+.*?local\.get \$wasm_compound_num_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const 5\s+i32\.add\s+call \$__sura_dict_put\s+local\.set \$wasm_compound_num_profile' -or
        $sourceCompoundWindow -notmatch '(?s)local\.get \$wasm_compound_num_profile\s+local\.get \$wasm_compound_num_key.*?local\.get \$wasm_compound_num_profile\s+local\.get \$wasm_compound_num_key\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 6\s+i32\.add\s+call \$__sura_dict_put\s+local\.set \$wasm_compound_num_profile') {
        throw "generated WAT should lower source numeric/string/local/index/dot/dynamic-key in-place operators through raw or tagged read-modify-write assignments"
    }
    $sourceFieldAssignIndex = $watText.IndexOf('local.set $wasm_field_assign_label')
    if ($sourceFieldAssignIndex -lt 0) {
        throw "generated WAT should include source field assignment label"
    }
    $sourceFieldAssignStart = [Math]::Max(0, $sourceFieldAssignIndex - 70000)
    $sourceFieldAssignWindow = $watText.Substring($sourceFieldAssignStart, $sourceFieldAssignIndex - $sourceFieldAssignStart)
    $sourceFieldSetCount = [regex]::Matches($sourceFieldAssignWindow, '(?s)local\.get \$wasm_field_values\s+i32\.const \d+.*?call \$__sura_dict_put\s+local\.set \$wasm_field_values').Count
    if ($sourceFieldSetCount -lt 6 -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_string.*?call \$__sura_value_type_name' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_string.*?call \$__sura_value_to_string' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_bool.*?call \$__sura_value_type_name' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_bool.*?call \$__sura_value_is_truthy' -or
        $sourceFieldAssignWindow -notmatch 'call \$__sura_value_nil\s+call \$__sura_value_type_name' -or
        $sourceFieldAssignWindow -notmatch 'call \$__sura_value_nil\s+call \$__sura_value_is_truthy' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_function.*?call \$__sura_value_type_name' -or
        $sourceFieldAssignWindow -notmatch '(?s)local\.get \$wasm_field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy') {
        throw "generated WAT should lower source field assignments for string/bool/nil/array/dict/function values and preserve later Value helpers"
    }
    $sourceIndexAssignIndex = $watText.IndexOf('local.set $wasm_index_assign_label')
    $sourceIndexAssignStart = $watText.IndexOf('local.set $wasm_index_values')
    if ($sourceIndexAssignIndex -lt 0 -or $sourceIndexAssignStart -lt 0 -or $sourceIndexAssignStart -ge $sourceIndexAssignIndex) {
        throw "generated WAT should include source index assignment label"
    }
    $sourceIndexAssignWindow = $watText.Substring($sourceIndexAssignStart, $sourceIndexAssignIndex - $sourceIndexAssignStart)
    $sourceIndexArrayStoreCount = [regex]::Matches($sourceIndexAssignWindow, '(?s)local\.get \$wasm_index_values\s+i32\.const \d+.*?call \$__sura_array_set_checked').Count
    $sourceIndexDictSetCount = [regex]::Matches($sourceIndexAssignWindow, '(?s)local\.get \$wasm_index_dict\s+local\.get \$wasm_index_key_\w+.*?call \$__sura_dict_put\s+local\.set \$wasm_index_dict').Count
    if ($sourceIndexArrayStoreCount -lt 6 -or
        $sourceIndexDictSetCount -lt 6 -or
        $sourceIndexAssignWindow -notmatch 'call \$__sura_value_bool\s+call \$__sura_value_is_truthy' -or
        $sourceIndexAssignWindow -notmatch 'call \$__sura_value_nil\s+call \$__sura_value_is_truthy' -or
        $sourceIndexAssignWindow -notmatch '(?s)call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $sourceIndexAssignWindow -notmatch '(?s)call \$__sura_value_array.*?call \$__sura_value_length' -or
        $sourceIndexAssignWindow -notmatch '(?s)call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $sourceIndexAssignWindow -notmatch '(?s)call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $sourceIndexAssignWindow -notmatch '(?s)call \$__sura_value_function.*?call \$__sura_value_type_name' -or
        $sourceIndexAssignWindow -notmatch '(?s)call \$__sura_value_function.*?call \$__sura_value_is_truthy') {
        throw "generated WAT should lower source index assignments for array and dynamic dict targets with later Value helpers"
    }
    if ($watText -notmatch '\(local \$__(?:ast_)?for_step' -or
        $watText -notmatch 'local\.set \$__(?:ast_)?for_step' -or
        $watText -notmatch 'range-for with step' -or
        $watText -notmatch 'i32\.lt_s') {
        throw "generated WAT should lower Sura stepped range-for loops"
    }
    if ($watText -notmatch 'tilde range-for' -or
        $watText -notmatch 'local\.set \$tilde_total') {
        throw "generated WAT should lower Sura tilde range-for loops"
    }
    if ($watText -notmatch 'local\.set \$when_tilde' -or
        $watText -notmatch 'i32\.ge_s' -or
        $watText -notmatch 'i32\.le_s') {
        throw "generated WAT should lower Sura tilde range when arms"
    }
    if ($watText -notmatch 'local\.set \$when_first_else' -or
        $watText -notmatch 'i32\.const 1') {
        throw "generated WAT should lower first-else when arms"
    }
    if ($watText -notmatch 'local\.set \$when_middle_else' -or
        $watText -notmatch 'local\.set \$when_matched_before_else') {
        throw "generated WAT should lower nonterminal else when arms with native matched-arm semantics"
    }
    if ($watText -match 'call \$assert_neq' -or
        ($watText | Select-String -Pattern 'i32\.eq' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should lower assert_ne/assert_neq without unresolved helper calls"
    }
    if ($watText -match 'call \$assert_between' -or
        $watText -notmatch 'assert/assert_eq/assert_ne/assert_type/assert_len/assert_between/assert_approx' -or
        ($watText | Select-String -Pattern 'i32\.lt_s' -AllMatches).Matches.Count -lt 2 -or
        ($watText | Select-String -Pattern 'i32\.gt_s' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should lower assert_between to numeric bounds checks"
    }
    if ($watText -match 'call \$assert_approx' -or
        ($watText | Select-String -Pattern 'i32\.sub' -AllMatches).Matches.Count -lt 2 -or
        $watText -notmatch 'i32\.gt_s') {
        throw "generated WAT should lower assert_approx to absolute-difference bounds checks"
    }
    if ($watText -match 'local\.get \$true' -or
        $watText -match 'local\.get \$false' -or
        $watText -notmatch 'true/false bool literals' -or
        ($watText | Select-String -Pattern 'i32\.const 1' -AllMatches).Matches.Count -lt 2 -or
        ($watText | Select-String -Pattern 'i32\.const 0' -AllMatches).Matches.Count -lt 2) {
        throw "generated WAT should lower true/false bool literals to numeric constants"
    }
    if ($watText -notmatch 'numeric enum declarations' -or
        $watText -match 'local\.get \$WasmMode' -or
        $watText -match 'WasmMode\.READY' -or
        ($watText | Select-String -Pattern 'i32\.const 7' -AllMatches).Matches.Count -lt 1 -or
        ($watText | Select-String -Pattern 'i32\.const 11' -AllMatches).Matches.Count -lt 1) {
        throw "generated WAT should lower numeric enum member access to constants"
    }
    if ($watText -notmatch 'string/bare enum declarations' -or
        $watText -match 'local\.get \$WasmLabel' -or
        $watText -match 'WasmLabel\.READY' -or
        $watText -notmatch 'local\.set \$enum_label' -or
        $watText -notmatch 'call \$__sura_string_concat') {
        throw "generated WAT should lower string and bare enum member access to string constants"
    }
    if ($watText -notmatch 'local\.set \$string_match_pick' -or
        $watText -notmatch '(?s)local\.get \$__ast_match\d+\s+call \$__sura_value_string.*?call \$__sura_value_eq.*?local\.set \$string_match_pick') {
        throw "generated WAT should lower string match arms through tagged Value equality"
    }
    if ($watText -notmatch 'local\.set \$bool_match_pick' -or
        $watText -notmatch 'local\.set \$nil_match_pick' -or
        $watText -notmatch '(?s)call \$__sura_value_bool.*?call \$__sura_value_eq.*?local\.set \$bool_match_pick' -or
        $watText -notmatch '(?s)call \$__sura_value_nil.*?call \$__sura_value_eq.*?local\.set \$nil_match_pick') {
        throw "generated WAT should lower bool and nil match arms through tagged Value equality"
    }
    if ($watText -notmatch 'local\.set \$match_join_label' -or
        $watText -notmatch '(?s)local\.get \$match_join_label.*?call \$__sura_string_concat.*?call \$__sura_value_string.*?call \$__sura_value_eq') {
        throw "generated WAT should merge same-type string match arm assignment hints after match"
    }
    $conflictAfterIndex = $watText.IndexOf('local.set $match_conflict_after')
    if ($conflictAfterIndex -lt 0) {
        throw "generated WAT should include match_conflict_after assignment"
    }
    $conflictWindowStart = [Math]::Max(0, $conflictAfterIndex - 260)
    $conflictWindow = $watText.Substring($conflictWindowStart, $conflictAfterIndex - $conflictWindowStart)
    if ($conflictWindow -match 'call \$__sura_string_concat' -or $conflictWindow -notmatch 'i32\.add') {
        throw "generated WAT should drop conflicting match arm assignment hints after match"
    }
    foreach ($conflictName in @("match_conflict_field_after", "match_conflict_index_after")) {
        $afterIndex = $watText.IndexOf("local.set `$$conflictName")
        if ($afterIndex -lt 0) {
            throw "generated WAT should include $conflictName assignment"
        }
        $windowStart = [Math]::Max(0, $afterIndex - 320)
        $window = $watText.Substring($windowStart, $afterIndex - $windowStart)
        if ($window -match 'call \$__sura_string_concat' -or $window -notmatch 'i32\.add') {
            throw "generated WAT should drop conflicting match arm access hints before $conflictName"
        }
    }
    if ($watText -notmatch '\(func \$wasm_pick_label \(export "wasm_pick_label"\)' -or
        $watText -notmatch '\(func \$wasm_pick_bool \(export "wasm_pick_bool"\)' -or
        $watText -notmatch '\(func \$wasm_pick_nil \(export "wasm_pick_nil"\)' -or
        $watText -notmatch '(?s)call \$wasm_pick_label.*?call \$__sura_value_string.*?call \$__sura_value_eq' -or
        $watText -notmatch '(?s)call \$wasm_pick_bool.*?call \$__sura_value_bool.*?call \$__sura_value_eq' -or
        $watText -notmatch '(?s)call \$wasm_pick_nil\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?end\s+local\.get \$__sura_wasm_call_tmp\s+drop.*?call \$__sura_value_nil') {
        throw "generated WAT should preserve string/bool/nil function return hints from normal source input"
    }
    if ($watText -notmatch 'local \$ternary_runtime_label i32' -or
        $watText -notmatch '(?s)local \$ternary_runtime_label i32.*?local\.get \$ternary_array_value.*?call \$__sura_value_array.*?call \$__sura_value_type_name.*?local\.get \$ternary_dict_value.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $watText -notmatch '(?s)local \$ternary_runtime_label i32.*?local\.get \$ternary_array_full_value.*?call \$__sura_value_array.*?local\.get \$ternary_dict_value.*?call \$__sura_value_dict.*?end\s+call \$__sura_value_length' -or
        $watText -notmatch '(?s)local \$ternary_runtime_label i32.*?call \$__sura_value_dynamic_dict\s+call \$__sura_value_is_truthy' -or
        $watText -notmatch '(?s)local \$ternary_runtime_label i32.*?call \$__sura_value_dynamic_array.*?call \$__sura_value_dynamic_dict\s+call \$__sura_value_to_string') {
        throw "generated WAT should lower dynamic ternary branches through Value type/length/truthiness/stringification helpers"
    }
    if ($watText -notmatch '\(func \$wasm_first_label_from_foreach \(export "wasm_first_label_from_foreach"\)' -or
        $watText -notmatch '(?s)local\.get \$label\s+return' -or
        $watText -notmatch '(?s)call \$wasm_first_label_from_foreach.*?call \$__sura_value_string.*?call \$__sura_value_eq' -or
        $watText -notmatch '(?s)call \$wasm_first_label_from_foreach.*?call \$__sura_string_concat') {
        throw "generated WAT should preserve string return hints from foreach loop variables"
    }
    if ($watText -notmatch '\(func \$wasm_label_from_match \(export "wasm_label_from_match"\)' -or
        $watText -notmatch '(?s)call \$wasm_label_from_match.*?call \$__sura_value_string.*?call \$__sura_value_eq' -or
        $watText -notmatch '(?s)call \$wasm_label_from_match.*?call \$__sura_string_concat') {
        throw "generated WAT should preserve string return hints from match arms"
    }
    if ($watText -notmatch '\(func \$wasm_label_from_if \(export "wasm_label_from_if"\)' -or
        $watText -notmatch '(?s)call \$wasm_label_from_if.*?call \$__sura_value_string.*?call \$__sura_value_eq' -or
        $watText -notmatch '(?s)call \$wasm_label_from_if.*?call \$__sura_string_concat') {
        throw "generated WAT should preserve same-type string return hints from if branches"
    }
    $ifMixedIndex = $watText.IndexOf('local.set $if_mixed_after')
    if ($ifMixedIndex -lt 0) {
        throw "generated WAT should include if_mixed_after assignment"
    }
    $ifMixedWindowStart = [Math]::Max(0, $ifMixedIndex - 260)
    $ifMixedWindow = $watText.Substring($ifMixedWindowStart, $ifMixedIndex - $ifMixedWindowStart)
    if ($ifMixedWindow -notmatch 'call \$__sura_value_add') {
        throw "generated WAT should preserve mixed if-branch returns through dynamic Value addition"
    }
    if ($watText -notmatch '\(func \$__sura_value_add ' -or
        $watText -notmatch '(?s)call \$wasm_mixed_from_if.*?call \$__sura_value_add\s+local\.set \$dynamic_num_add' -or
        $watText -notmatch '(?s)call \$wasm_mixed_from_if.*?call \$__sura_value_add\s+local\.set \$dynamic_string_add') {
        throw "generated WAT should route runtime-selected numeric and string addition through the Value ABI"
    }
    foreach ($dynamicNumericCase in @(
        @{ Local = 'dynamic_sub'; Op = 'call \$__sura_value_sub'; Tagged = $true },
        @{ Local = 'dynamic_mul'; Op = 'call \$__sura_value_mul'; Tagged = $true },
        @{ Local = 'dynamic_div'; Op = 'call \$__sura_value_div'; Tagged = $true },
        @{ Local = 'dynamic_mod'; Op = 'call \$__sura_value_mod'; Tagged = $true },
        @{ Local = 'dynamic_lt'; Op = 'call \$__sura_value_numeric_compare'; Tagged = $true },
  @{ Local = 'dynamic_shift'; Op = 'call \$__sura_value_bitwise'; Tagged = $true }
    )) {
        $localIndex = $watText.IndexOf('local.set $' + $dynamicNumericCase.Local)
        if ($localIndex -lt 0) {
            throw "generated WAT should include $($dynamicNumericCase.Local)"
        }
        $windowStart = [Math]::Max(0, $localIndex - 420)
        $window = $watText.Substring($windowStart, $localIndex - $windowStart)
        if ($window -notmatch $dynamicNumericCase.Op) {
            throw "generated WAT should use the expected dynamic numeric operator before $($dynamicNumericCase.Local)"
        }
        if ([bool]$dynamicNumericCase.Tagged) {
            if ($window -notmatch 'local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown') {
                throw "generated WAT should guard tagged numeric failures before $($dynamicNumericCase.Local)"
            }
        } elseif ($window -notmatch 'call \$__sura_value_require_num') {
            throw "generated WAT should require an integer Value before $($dynamicNumericCase.Local)"
        }
    }
    if ($watText -notmatch 'local\.set \$dynamic_while_score' -or
        $watText -notmatch '(?s)call \$__sura_value_string_or_nil\s+call \$__sura_value_is_truthy.*?local\.set \$dynamic_while_score') {
        throw "generated WAT should lower string while conditions through tagged Value truthiness"
    }

    $astSource = Join-Path $temp "ast_wasm_target.sura"
    $astImportSource = Join-Path $temp "ast_wasm_import.sura"
    $astJson = Join-Path $temp "ast_wasm_target.json"
    $astWat = Join-Path $temp "ast_wasm_target.wat"
    $astWasm = Join-Path $temp "ast_wasm_target.wasm"
@'
func imported_ast(n) do
  return n * 2
end
'@ | Set-Content -LiteralPath $astImportSource -Encoding UTF8
@'
import "ast_wasm_import.sura"

enum AstWasmMode do
  READY is 7
end

func add_ast(a, b) do
  return a + b
end

func label_pair(prefix, value) do
  return prefix + value
end

func nested_label_pair(prefix, value) do
  inner is label_pair(prefix, value)
  return inner
end

func nested_method_label(value) do
  local_thrower is new AstMethodThrower()
  echoed is local_thrower.echo_arg(value)
  return "Nested method {type(echoed)} {echoed}"
end

func if_local_method_label(flag, value) do
  if flag then
    selected is new AstMethodThrower()
  else
    selected is new AstMethodThrower()
  end
  echoed is selected.echo_arg(value)
  return "If method {type(echoed)} {echoed}"
end

func match_local_method_label(mode, value) do
  match mode
  when 1 then
      selected is new AstMethodThrower()
  when _ then
      selected is new AstMethodThrower()
  end
  echoed is selected.echo_arg(value)
  return "Match method {type(echoed)} {echoed}"
end

func bool_state_line(prefix, active, missing) do
  return prefix + active + " " + missing
end

func bool_gate_ast(active, missing) do
  return active and not missing
end

func throw_text_ast() do
  throw "fn boom"
end

func throw_number_ast() do
  throw 12
end

func conflict_probe(value) do
  return value + 1
end

func sum_to_ast(n) do
  total is 0
  for i in 1 to n do
    total += i
  end
  return total
end

block_double_ast is func(value) do
  return value * 2
end

func triple_ast(value) do
  return value * 3
end

func zero_a_ast() do
  return 11
end

func zero_b_ast() do
  return 17
end

func fib(n) do
  if n <= 1 then return n
  return fib(n - 1) + fib(n - 2)
end

func return_direct_function() do
  return fib
end

func call_dynamic_zero_function_ast(flag) do
  handler is flag ? zero_a_ast : zero_b_ast
  return handler()
end

func add_pair_ast(left, right) do
  return left + right
end

func mul_pair_ast(left, right) do
  return left * right
end

func sum_triple_ast(left, middle, right) do
  return left + middle + right
end

func mul_add_triple_ast(left, middle, right) do
  return left * middle + right
end

func sum_quad_ast(a, b, c, d) do
  return a + b + c + d
end

func mul_add_quad_ast(a, b, c, d) do
  return a * b + c + d
end

func sum_five_ast(a, b, c, d, e) do
  return a + b + c + d + e
end

func mul_add_five_ast(a, b, c, d, e) do
  return a * b + c + d + e
end

func sum_eight_ast(a, b, c, d, e, f, g, h) do
  return a + b + c + d + e + f + g + h
end

func mul_add_eight_ast(a, b, c, d, e, f, g, h) do
  return a * b + c + d + e + f + g + h
end

func prefix_a_ast(value) do
  return "A" + value
end

func prefix_b_ast(value) do
  return "B" + value
end

func positive_ast(value) do
  return value > 0
end

func over_ten_ast(value) do
  return value > 10
end

func pair_array_a_ast(value) do
  return [value, "a"]
end

func pair_array_b_ast(value) do
  return [value + 1, "b"]
end

func nil_a_ast(value) do
  return nil
end

func nil_b_ast(value) do
  return nil
end

func profile_a_ast(value) do
  return {score: value, name: "a"}
end

func profile_b_ast(value) do
  return {score: value + 1, name: "b"}
end

func return_add_pair_ast(value) do
  return add_pair_ast
end

func return_mul_pair_ast(value) do
  return mul_pair_ast
end

format_pair_ast is func(prefix, value) do
  return prefix + value
end

func describe_function_ast(handler) do
  return "Func {type(handler)} {to_bool(handler)}"
end

func pass_function_ast(handler) do
  return handler
end

func call_function_param_ast(handler, value) do
  return handler(value)
end

func describe_call_function_param_ast(handler, value) do
  return "ParamFn {type(handler)} {to_str(handler)} {handler(value)}"
end

func call_dynamic_function_param_ast(handler, value) do
  return handler(value)
end

func call_dynamic_local_function_ast(flag, value) do
  handler is flag ? block_double_ast : triple_ast
  return handler(value)
end

func call_dynamic_binary_function_ast(flag, left, right) do
  handler is flag ? add_pair_ast : mul_pair_ast
  return handler(left, right)
end

func call_dynamic_triple_function_ast(flag, left, middle, right) do
  handler is flag ? sum_triple_ast : mul_add_triple_ast
  return handler(left, middle, right)
end

func call_dynamic_quad_function_ast(flag, a, b, c, d) do
  handler is flag ? sum_quad_ast : mul_add_quad_ast
  return handler(a, b, c, d)
end

func call_dynamic_five_function_ast(flag, a, b, c, d, e) do
  handler is flag ? sum_five_ast : mul_add_five_ast
  return handler(a, b, c, d, e)
end

func call_dynamic_eight_function_ast(flag, a, b, c, d, e, f, g, h) do
  handler is flag ? sum_eight_ast : mul_add_eight_ast
  return handler(a, b, c, d, e, f, g, h)
end

func call_dynamic_string_function_ast(flag, value) do
  handler is flag ? prefix_a_ast : prefix_b_ast
  return handler(value)
end

func call_dynamic_bool_function_ast(flag, value) do
  handler is flag ? positive_ast : over_ten_ast
  return handler(value)
end

func call_dynamic_array_function_ast(flag, value) do
  handler is flag ? pair_array_a_ast : pair_array_b_ast
  return handler(value)
end

func call_dynamic_nil_function_ast(flag, value) do
  handler is flag ? nil_a_ast : nil_b_ast
  return handler(value)
end

func call_dynamic_dict_return_function_ast(flag, value) do
  handler is flag ? profile_a_ast : profile_b_ast
  return handler(value)
end

func call_dynamic_function_return_function_ast(flag, value) do
  handler is flag ? return_add_pair_ast : return_mul_pair_ast
  return handler(value)
end

func call_dynamic_dict_lookup_dict_return_function_ast(flag, value) do
  handlers is {first: profile_a_ast, second: profile_b_ast}
  key is flag ? "first" : "second"
  handler is handlers[key]
  return handler(value)
end

func call_dynamic_array_lookup_function_return_function_ast(flag, value) do
  handlers is [return_add_pair_ast, return_mul_pair_ast]
  index is flag ? 0 : 1
  handler is handlers[index]
  return handler(value)
end

func call_dynamic_binary_dict_function_ast(flag, left, right) do
  handlers is {add: add_pair_ast, mul: mul_pair_ast}
  key is flag ? "add" : "mul"
  handler is handlers[key]
  return handler(left, right)
end

func call_dynamic_binary_array_function_ast(flag, left, right) do
  handlers is [add_pair_ast, mul_pair_ast]
  index is flag ? 0 : 1
  handler is handlers[index]
  return handler(left, right)
end

func call_foreach_binary_function_ast(flag, left, right) do
  handlers is [add_pair_ast, mul_pair_ast]
  selected is flag ? add_pair_ast : mul_pair_ast
  for handler in handlers do
    if handler == selected then
      return handler(left, right)
    end
  end
  return 0
end

func call_dynamic_binary_param_function_ast(handler, left, right) do
  return handler(left, right)
end

func call_dynamic_dict_function_key_ast(flag, value) do
  handlers is {double: block_double_ast, triple: triple_ast}
  key is flag ? "double" : "triple"
  handler is handlers[key]
  return handler(value)
end

func local_function_value_ast() do
  local_inner_ast is func(value) do
    return value + 5
  end
  local_alias_ast is local_inner_ast
  local_direct_call_ast is local_inner_ast(7)
  local_alias_call_ast is local_alias_ast(8)
  local_handlers_ast is [local_inner_ast]
  local_map_ast is {handler: local_inner_ast}
  return "LocalFn {type(local_inner_ast)} {to_str(local_alias_ast)} {local_direct_call_ast} {local_alias_call_ast} {to_bool(local_handlers_ast[0])} {type(local_map_ast.handler)}"
end

func inline_function_expr_values_ast() do
  inline_handlers_ast is [func(value) do
    return value + 4
  end]
  inline_map_ast is {handler: func(value) do
    return value + 5
  end}
  return "InlineFn {type(inline_handlers_ast[0])} {to_bool(inline_handlers_ast[0])} {to_str(inline_handlers_ast[0])} {type(inline_map_ast.handler)} {to_bool(inline_map_ast.handler)} {to_str(inline_map_ast.handler)}"
end

func direct_inline_function_expr_values_ast() do
  return "DirectInlineFn {type(func(value) do
    return value + 6
  end)} {to_bool(func(value) do
    return value + 7
  end)} {to_str(func(value) do
    return value + 8
  end)}"
end

func returned_inline_function_value_ast() do
  return func(value) do
    return value + 9
  end
end

func captured_inline_function_value_ast() do
  bonus is 12
  handler is func(value) do
    return value + bonus
  end
  return handler(5)
end

func captured_inline_function_param_ast(bonus) do
  handler is func(value) do
    return value + bonus
  end
  return handler(5)
end

func captured_inline_function_alias_ast() do
  bonus is 14
  alias is bonus
  adjusted is alias + 3
  handler is func(value) do
    return value + adjusted
  end
  return handler(6)
end

func captured_inline_function_text_ast() do
  prefix is "Sura"
  suffix is "Lang"
  is_sura is prefix == "Sura"
  missing is nil
  fallback is missing ?? suffix
  phrase is is_sura ? prefix + fallback : "Bad"
  raw is " sura "
  clean is raw.trim()
  shout is clean.upper()
  quiet is shout.lower()
  has_ur is quiet.contains("ur")
  starts_su is quiet.startsWith("su")
  ends_ra is quiet.endsWith("ra")
  pos_ra is quiet.indexOf("ra")
  swapped is quiet.replace("ur", "UR")
  middle is quiet.substring(1, 3)
  tail is quiet.slice(-2)
  low is min(4, -2, 8)
  high is max(4, 12, 8)
  power is pow(2, 3)
  root is sqrt(17)
  limited is clamp(15, 0, 9)
  rounded is round(2.6)
  module_upper is string.upper(clean)
  call_has is contains(quiet, "ur")
  call_tail is substring(quiet, 2)
  direct_index_camel is string_indexOf(quiet, "ur")
  direct_starts_camel is string_startsWith(quiet, "su")
  direct_ends_camel is string_endsWith(quiet, "ra")
  handler is func() do
    return "{phrase} {clean} {shout} {quiet} {has_ur} {starts_su} {ends_ra} {pos_ra} {swapped} {middle} {tail} Math {low} {high} {power} {root} {limited} {rounded} Calls {module_upper} {call_has} {call_tail} Direct {direct_index_camel} {direct_starts_camel} {direct_ends_camel}"
  end
  return handler()
end

func captured_inline_function_bool_ast() do
  base is 14
  adjusted is base + 3
  bad is adjusted != 17
  ok_base is not bad
  ok is ok_base and true
  handler is func() do
    return ok
  end
  return handler()
end

func captured_inline_function_if_merge_ast(flag) do
  suffix is "Base"
  if flag then
    suffix is "Lang"
  else
    suffix is "Lang"
  end
  handler is func() do
    return suffix
  end
  return handler()
end

func captured_inline_function_match_merge_ast(value) do
  suffix is "Base"
  match value
    when 1 then
      suffix is "Match"
    when _ then
      suffix is "Match"
  end
  handler is func() do
    return suffix
  end
  return handler()
end

func captured_inline_function_collection_ast() do
  nums is [10, 12]
  profile is {name: "Sura", city: "Seoul"}
  picked is nums[1]
  name is profile.name
  city is profile["city"]
  count is length(nums)
  kind is type(profile)
  ready is to_bool(profile)
  total is array.sum(nums)
  avg is array.avg(nums)
  low_num is nums.min()
  has_twelve is nums.contains(12)
  pos_ten is nums.index_of(10)
  seq is array.range(2, 7, 2)
  nums_text is to_str(nums)
  profile_text is to_str(profile)
  headline is "Snapshot {picked} {name} {nums} {profile}"
  handler is func() do
    return "Collection {picked} {name} {city} {count} {kind} {ready} {total} {avg} {low_num} {has_twelve} {pos_ten} {seq} {nums_text} {profile_text} {headline}"
  end
  return handler()
end

func returned_captured_inline_function_ast() do
  bonus is 13
  return func(value) do
    return value + bonus
  end
end

func returned_param_captured_inline_function_ast(bonus) do
  return func(value) do
    return value + bonus
  end
end

func returned_param_function_capture_ast(handler) do
  return func(value) do
    return handler(value)
  end
end

func returned_param_alias_captured_inline_function_ast(bonus) do
  adjusted is bonus + 1
  return func(value) do
    return value + adjusted
  end
end

func returned_param_if_alias_captured_inline_function_ast(flag, bonus) do
  adjusted is 0
  if flag then
    adjusted is bonus + 1
  else
    adjusted is bonus + 1
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_if_false_no_else_captured_inline_function_ast(flag, bonus) do
  adjusted is bonus + 1
  if flag then
    adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_if_unknown_no_else_captured_inline_function_ast(bonus) do
  adjusted is bonus + 1
  if bonus then
    marker is 1
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_match_alias_captured_inline_function_ast(kind, bonus) do
  adjusted is 0
  match kind
    when 1 then
      adjusted is bonus + 1
    when _ then
      adjusted is bonus + 1
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_match_no_arm_captured_inline_function_ast(kind, bonus) do
  adjusted is bonus + 1
  match kind
    when 1 then
      adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_repeat_alias_captured_inline_function_ast(bonus) do
  adjusted is 0
  repeat 1 do
    adjusted is bonus + 1
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_repeat_accum_captured_inline_function_ast(bonus) do
  adjusted is bonus
  repeat 3 do
    adjusted is adjusted + 2
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_for_accum_captured_inline_function_ast(bonus) do
  adjusted is bonus
  for i in 2 to 6 step 2 do
    adjusted is adjusted + i
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_for_zero_captured_inline_function_ast(bonus) do
  adjusted is bonus + 1
  for i in 5 to 2 step 1 do
    adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_foreach_accum_captured_inline_function_ast(bonus) do
  adjusted is bonus
  for label, item in {a: 2, b: 4, c: 6} do
    adjusted is adjusted + item
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_foreach_empty_captured_inline_function_ast(bonus) do
  adjusted is bonus + 1
  for item in [] do
    adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_foreach_empty_dict_captured_inline_function_ast(bonus) do
  adjusted is bonus + 1
  for key, item in {} do
    adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_while_false_captured_inline_function_ast(flag, bonus) do
  adjusted is bonus + 1
  while flag do
    adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func returned_param_while_literal_false_captured_inline_function_ast(bonus) do
  adjusted is bonus + 1
  while false do
    adjusted is adjusted + 100
  end
  return func(value) do
    return value + adjusted
  end
end

func control_flow_function_alias_ast(flag) do
  handler is block_double_ast
  if flag then
    handler is block_double_ast
  else
    handler is block_double_ast
  end
  return handler(9)
end

func loop_function_alias_ast() do
  handler is block_double_ast
  repeat 1 do
    handler is block_double_ast
  end
  return handler(10)
end

func match_function_alias_ast(value) do
  handler is block_double_ast
  match value
    when 1 then
      handler is block_double_ast
    when _ then
      handler is block_double_ast
  end
  return handler(11)
end

func pick_function_ast(flag) do
  return flag ? block_double_ast : format_pair_ast
end

func pick_function_match_ast(choice) do
  match choice
    when 1 then
      return block_double_ast
    when 2 then
      return format_pair_ast
    when _ then
      return block_double_ast
  end
end

func throw_function_ast() do
  throw block_double_ast
end

func throw_selected_function_ast(handler) do
  selected_handler is handler
  throw selected_handler
end

func pick_label_ast(flag) do
  return flag ? "on" : "off"
end

func pick_bool_ast(flag) do
  return flag ? true : false
end

func make_values_ast(flag) do
  return flag ? [1] : []
end

func make_mixed_values_ast() do
  values is [32, "ok", true, nil]
  return values
end

func make_mixed_profile_ast() do
  info is {score: 32, name: "ok", active: true, missing: nil}
  return info
end

func make_value_collection_alias_ast(flag) do
  return flag ? ["zero", "one"] : {name: "bad"}
end

func make_value_profile_alias_ast(flag, score_value) do
  return flag ? {name: "Ada", score: score_value} : ["bad"]
end

func make_profile_ast(flag) do
  return flag ? {ready: 1} : {}
end

func pass_values_ast(items) do
  return items
end

func pass_profile_ast(info) do
  return info
end

func choose_values_ast(flag, items) do
  return flag ? items : []
end

func choose_profile_ast(flag, info) do
  return flag ? info : {}
end

func choose_collection_profile_key_ast(flag) do
  if flag then
    return "bonus"
  end
  return "score"
end

func function_alias_values_text_ast(items) do
  picked is choose_values_ast(true, items)
  return to_str(picked)
end

func function_alias_empty_values_text_ast(items) do
  picked is choose_values_ast(false, items)
  return to_str(picked)
end

func function_alias_profile_text_ast(info) do
  picked is choose_profile_ast(true, info)
  return to_str(picked)
end

func function_alias_empty_profile_text_ast(info) do
  picked is choose_profile_ast(false, info)
  return to_str(picked)
end

func function_alias_profile_bonus_text_ast(info, key) do
  picked is choose_profile_ast(true, info)
  return to_str(picked[key])
end

func choose_values_local_ast(flag, items) do
  picked is flag ? items : []
  return picked
end

func choose_profile_local_ast(flag, info) do
  picked is flag ? info : {}
  return picked
end

func choose_values_local_gate_ast(flag, items) do
  picked is flag ? items : []
  return picked and true
end

func choose_profile_local_gate_ast(flag, info) do
  picked is flag ? info : {}
  return picked or false
end

func local_tagged_kind_len_ast(flag) do
  bot is flag ? new AstTagged() : new AstTagged()
  label is bot.kind_text()
  return label.len()
end

func local_tagged_field_len_ast(flag) do
  bot is flag ? new AstTagged() : new AstTagged()
  label is bot.kind + " " + bot.active + " " + bot.missing
  return label.len()
end

func local_tagged_gate_ast(flag) do
  bot is flag ? new AstTagged() : new AstTagged()
  return bot.active_gate()
end

func make_tagged_local_ast() do
  bot is new AstTagged()
  return bot
end

func choose_tagged_local_ast(flag) do
  bot is flag ? make_tagged_local_ast() : make_tagged_local_ast()
  return bot
end

func choose_tagged_if_ast(flag) do
  if flag then
    return new AstTagged()
  else
    return new AstTagged()
  end
end

func choose_tagged_match_ast(mode) do
  match mode
  when 1 then
    return new AstTagged()
  when _ then
    return new AstTagged()
  end
end

func local_tagged_from_call_len_ast(flag) do
  bot is choose_tagged_local_ast(flag)
  label is bot.kind_text()
  return label.len()
end

func local_tagged_from_if_len_ast(flag) do
  bot is choose_tagged_if_ast(flag)
  label is bot.kind_text()
  return label.len()
end

func local_tagged_from_match_len_ast(mode) do
  bot is choose_tagged_match_ast(mode)
  label is bot.kind_text()
  return label.len()
end

func local_tagged_from_method_len_ast(flag) do
  bot is choose_tagged_local_ast(flag)
  peer is bot.peer()
  label is peer.kind_text()
  return label.len()
end

func tagged_param_kind_len_ast(bot) do
  label is bot.kind_text()
  return label.len()
end

func tagged_param_peer_len_ast(bot) do
  peer is bot.peer()
  label is peer.kind_text()
  return label.len()
end

func tagged_param_field_label_ast(bot) do
  return bot.kind
end

func tagged_param_index_label_ast(bot) do
  return bot["kind"]
end

func direct_tagged_call_len_ast() do
  label is make_tagged_local_ast().kind_text()
  return label.len()
end

func direct_tagged_ternary_len_ast(flag) do
  label is (flag ? new AstTagged() : new AstTagged()).kind_text()
  return label.len()
end

func direct_tagged_receiver_arg_len_ast() do
  return make_tagged_local_ast().peer_kind_len(new AstTagged())
end

func direct_tagged_index_label_ast() do
  return make_tagged_local_ast()["kind"] + "!"
end

func direct_tagged_ternary_index_label_ast(flag) do
  return "State " + (flag ? new AstTagged() : new AstTagged())["active"] + " " + (flag ? new AstTagged() : new AstTagged())["missing"]
end

func foreach_return_tagged_ast() do
  local_tagged_items is [new AstTagged(), new AstTagged()]
  for local_tagged_item in local_tagged_items do
    return local_tagged_item
  end
  return new AstTagged()
end

func foreach_return_tagged_dict_ast() do
  local_tagged_map is {primary: new AstTagged(), secondary: new AstTagged()}
  for local_tagged_name, local_tagged_value in local_tagged_map do
    return local_tagged_value
  end
  return new AstTagged()
end

class AstPoint do
  func init(x, y) do
    self.x is x
    self.y is y
  end
  func sum() do
    return self.x + self.y
  end
  func shifted(delta) do
    return self.x + self.y + delta
  end
  func shifted_conflict(delta) do
    return self.x + delta
  end
end

class AstDynamicBase do
  func init(amount) do
    self.amount is amount
  end
  func score(delta) do
    return self.amount + delta
  end
end

class AstDynamicScale extends AstDynamicBase do
  func init(amount) do
    self.amount is amount
  end
  func score(delta) do
    return self.amount * delta
  end
end

class AstDynamicInherited extends AstDynamicBase do
  func init(amount) do
    self.amount is amount
  end
end

func dynamic_method_score_ast(flag) do
  return (flag ? new AstDynamicBase(11) : new AstDynamicScale(7)).score(3)
end

func dynamic_inherited_score_ast(flag) do
  return (flag ? new AstDynamicBase(9) : new AstDynamicInherited(12)).score(2)
end

class AstBase do
  func init(root) do
    self.root is root
  end
  func root_value() do
    return self.root
  end
  func root_plus(delta) do
    return self.root + delta
  end
  func root_label() do
    return "Root " + self.root
  end
end

class AstChild extends AstBase do
  func init(root, leaf) do
    super.init(root)
    self.leaf is leaf
  end
  func root_plus_one() do
    return super.root_value() + 1
  end
  func root_plus_delta(delta) do
    return super.root_plus(delta)
  end
  func root_label_child() do
    return super.root_label() + "!"
  end
end

class AstForwardLabelBox do
  func init(name) do
    self.name is name
  end
  func label() do
    return self.name + "!"
  end
end

class AstForwardParent do
  func forwarded_label(box) do
    return box.label()
  end
end

class AstForwardChild extends AstForwardParent do
  func child_label_type(box) do
    return "SuperObjParamType {type(super.forwarded_label(box))}"
  end
end

class AstForwardBag do
  func init(items) do
    self.items is items
  end
  func values() do
    return self.items
  end
end

class AstForwardArrayParent do
  func forwarded_values(bag) do
    return bag.values()
  end
end

class AstForwardArrayChild extends AstForwardArrayParent do
  func child_values_len(bag) do
    return "SuperObjArrayLen {length(super.forwarded_values(bag))}"
  end
end

class AstFunctionHolder do
  func init(handler) do
    self.handler is handler
    self.backup is format_pair_ast
  end
  func handler_type() do
    return type(self.handler)
  end
  func handler_truth() do
    return to_bool(self.handler)
  end
  func handler_value() do
    return self.handler
  end
  func choose_handler(flag) do
    return flag ? self.handler : self.backup
  end
  func choose_handler_match(choice) do
    match choice
      when 1 then
        return self.handler
      when 2 then
        return self.backup
      when _ then
        return self.handler
    end
  end
  func call_handler(value) do
    picked is self.handler
    return picked(value)
  end
  func dict_handler_call(flag, value) do
    handlers is {double: block_double_ast, triple: triple_ast}
    key is flag ? "double" : "triple"
    handler is handlers[key]
    return handler(value)
  end
  func dict_handler_profile(flag, value) do
    handlers is {first: profile_a_ast, second: profile_b_ast}
    key is flag ? "first" : "second"
    handler is handlers[key]
    return handler(value)
  end
  func array_handler_function(flag, value) do
    handlers is [return_add_pair_ast, return_mul_pair_ast]
    index is flag ? 0 : 1
    handler is handlers[index]
    return handler(value)
  end
  func inline_handler_value() do
    return func(value) do
      return value + 10
    end
  end
  func choose_inline_handler(flag) do
    return flag ? func(value) do
      return value + 11
    end : self.backup
  end
  func handler_label() do
    return "Handler {type(self.handler)} {to_bool(self.handler)}"
  end
  func same_handler(other) do
    return self.handler == other
  end
end

class AstBinaryFunctionHolder do
  func init(handler) do
    self.handler is handler
    self.backup is mul_pair_ast
  end
  func call_handler(left, right) do
    picked is self.handler
    return picked(left, right)
  end
  func choose_handler(flag) do
    return flag ? self.handler : self.backup
  end
  func dict_handler_call(flag, left, right) do
    handlers is {add: add_pair_ast, mul: mul_pair_ast}
    key is flag ? "add" : "mul"
    handler is handlers[key]
    return handler(left, right)
  end
end

class AstBinaryFunctionChild extends AstBinaryFunctionHolder do
  func init(handler) do
    super.init(handler)
  end
  func inherited_handler() do
    return super.choose_handler(false)
  end
  func inherited_call(left, right) do
    picked is super.choose_handler(false)
    return picked(left, right)
  end
end

class AstFunctionChild extends AstFunctionHolder do
  func init(handler) do
    super.init(handler)
  end
  func inherited_handler() do
    return super.handler_value()
  end
  func inherited_label() do
    inherited is super.handler_value()
    return "Inherited {type(inherited)} {to_bool(inherited)}"
  end
  func inherited_inline_handler() do
    return super.inline_handler_value()
  end
  func inherited_inline_label(value) do
    picked is super.inline_handler_value()
    return "SuperInline {type(picked)} {to_str(picked)} {picked(value)}"
  end
  func inherited_inline_choice_label(value) do
    picked is super.choose_inline_handler(true)
    return "SuperInlineChoice {type(picked)} {to_str(picked)} {picked(value)}"
  end
end

class AstMethodThrower do
  func init() do
    self.ready is 1
  end
  func throw_text() do
    throw "method boom"
  end
  func throw_function_value() do
    throw block_double_ast
  end
  func echo_arg(value) do
    return value
  end
end

class AstSuperMethodThrower extends AstMethodThrower do
  func init() do
    super.init()
  end
  func catch_super_text() do
    result is ""
    try
      ignored_super_text is super.throw_text()
    catch err
      result is err + " via super"
    end
    return result
  end
  func catch_super_function_value() do
    result is ""
    try
      ignored_super_function is super.throw_function_value()
    catch err
      result is "Super function throw {type(err)} {to_bool(err)} {err(20)}"
    end
    return result
  end
  func super_echo_label(value) do
    echoed is super.echo_arg(value)
    return "Super echo {type(echoed)} {echoed}"
  end
end

class AstTagged do
  func init() do
    self.kind is "bot"
    self.active is true
    self.missing is nil
  end
  func kind_text() do
    return self.kind
  end
  func is_active() do
    return self.active
  end
  func none_value() do
    return self.missing
  end
  func kind_badge() do
    return self.kind + " badge"
  end
  func state_line() do
    return "State " + self.active + " " + self.missing
  end
  func active_gate() do
    return self.active and not self.missing
  end
  func kind_suffix(suffix) do
    return self.kind + suffix
  end
  func state_prefix(prefix) do
    return prefix + self.active + " " + self.missing
  end
  func peer() do
    return new AstTagged()
  end
  func peer_kind_len(other) do
    label is other.kind_text()
    return label.len()
  end
  func peer_kind_label(other) do
    return other.kind_text()
  end
  func peer_kind_field_label(other) do
    return other.kind
  end
  func peer_kind_index_label(other) do
    return other["kind"]
  end
end

class AstParamTagged do
  func init(kind, active, missing) do
    self.kind is kind
    self.active is active
    self.missing is missing
  end
  func kind_text() do
    return self.kind
  end
  func is_active() do
    return self.active
  end
  func none_value() do
    return self.missing
  end
  func kind_badge() do
    return self.kind + " badge"
  end
  func state_line() do
    return "State " + self.active + " " + self.missing
  end
  func active_gate() do
    return self.active and not self.missing
  end
  func kind_suffix(suffix) do
    return self.kind + suffix
  end
  func state_prefix(prefix) do
    return prefix + self.active + " " + self.missing
  end
end

class AstTaggedBox do
  func init(peer) do
    self.peer is peer
  end
  func peer_kind_label() do
    return self.peer.kind_text()
  end
  func peer_kind_field_label() do
    return self.peer.kind
  end
  func peer_kind_index_label() do
    return self.peer["kind"]
  end
end

class AstTaggedBoxChild extends AstTaggedBox do
  func init(peer) do
    super.init(peer)
  end
  func child_peer_kind_label() do
    return self.peer.kind_text()
  end
  func child_peer_kind_field_label() do
    return self.peer.kind
  end
  func child_peer_kind_index_label() do
    return self.peer["kind"]
  end
end

class AstCollectionTagged do
  func init(items, profile) do
    self.full_items is items
    self.empty_items is []
    self.full_profile is profile
    self.empty_profile is {}
    ctor_items is items
    ctor_profile is profile
    self.ctor_items_text is to_str(ctor_items)
    self.ctor_profile_bonus is ctor_profile["bonus"]
  end
  func full_items_value() do
    return self.full_items
  end
  func empty_items_value() do
    return self.empty_items
  end
  func full_profile_value() do
    return self.full_profile
  end
  func empty_profile_value() do
    return self.empty_profile
  end
  func pick_items(flag) do
    return flag ? self.full_items : self.empty_items
  end
  func pick_profile(flag) do
    return flag ? self.full_profile : self.empty_profile
  end
  func profile_key(flag) do
    if flag then
      return "bonus"
    end
    return "score"
  end
  func item_index(flag) do
    if flag then
      return 1
    end
    return 0
  end
  func pick_mixed_value_items(flag) do
    return flag ? ["zero", "one"] : {name: "bad"}
  end
  func pick_mixed_value_profile(flag) do
    return flag ? {name: "Ada", score: 16} : ["bad"]
  end
  func pick_items_local(flag) do
    picked is flag ? self.full_items : self.empty_items
    return picked
  end
  func pick_profile_local(flag) do
    picked is flag ? self.full_profile : self.empty_profile
    return picked
  end
  func pick_items_local_gate(flag) do
    picked is flag ? self.full_items : self.empty_items
    return picked and true
  end
  func pick_profile_local_gate(flag) do
    picked is flag ? self.full_profile : self.empty_profile
    return picked or false
  end
  func ctor_param_summary() do
    return self.ctor_items_text + " " + self.ctor_profile_bonus
  end
end

class AstCollectionParent do
  func init(items, profile) do
    super_items is items
    super_profile is profile
    self.super_items_text is to_str(super_items)
    self.super_profile_bonus is super_profile["bonus"]
  end
  func super_ctor_summary() do
    return self.super_items_text + " " + self.super_profile_bonus
  end
  func describe_parent(items, profile) do
    parent_items is items
    parent_profile is profile
    return to_str(parent_items) + " " + parent_profile["bonus"]
  end
  func values_parent(items, profile) do
    parent_items is items
    return parent_items
  end
  func profile_parent(items, profile) do
    parent_profile is profile
    return parent_profile
  end
  func mixed_items_parent(flag) do
    return flag ? ["zero", "one"] : {name: "bad"}
  end
  func mixed_profile_parent(flag) do
    return flag ? {name: "Ada", score: 16} : ["bad"]
  end
end

class AstCollectionChild extends AstCollectionParent do
  func init(items, profile) do
    super.init(items, profile)
  end
  func describe_via_super(items, profile) do
    return super.describe_parent(items, profile)
  end
  func values_via_super(items, profile) do
    return super.values_parent(items, profile)
  end
  func profile_via_super(items, profile) do
    return super.profile_parent(items, profile)
  end
  func mixed_items_via_super(flag) do
    return super.mixed_items_parent(flag)
  end
  func mixed_profile_via_super(flag) do
    return super.mixed_profile_parent(flag)
  end
end

class AstFieldConflict do
  func init(value) do
    self.value is value
  end
  func bump() do
    return self.value + 1
  end
end

class AstRecursive do
  func init(seed) do
    self.seed is seed
  end
  func countdown(n) do
    if n > 0 then
      return self.countdown(n - 1)
    else
      return self.seed
    end
  end
end

values is [1, 2, 3]
meta is {score: 9, bonus: 3}
profile is {name: "Ada", city: "Seoul"}
flags is {active: true, missing: nil}
names is ["Ada", "Grace"]
states is [true, false, nil]
bools is [true, false]
push_values is [1, 2]
push_values.push(3)
push_len is push_values.len()
push_tail is push_values[2]
push_label is "Push {push_len} {push_tail}"
pop_scores is [4, 5, 6]
pop_tail is pop_scores.pop()
pop_len is pop_scores.len()
pop_names is ["Ada", "Grace"]
pop_name is pop_names.pop()
pop_name_len is pop_names.len()
pop_flags is [true, false]
pop_flag is array.pop(pop_flags)
pop_flag_len is pop_flags.len()
pop_label is "Pop {pop_tail} {pop_len} {pop_name} {pop_name_len} {pop_flag} {pop_flag_len}"
reverse_values is [1, 2, 3]
reverse_result is reverse_values.reverse()
reverse_values_label is reverse_values.join(":")
reverse_result_label is reverse_result.join(":")
reverse_names is ["Ada", "Grace"]
reverse_names_result is array.reverse(reverse_names)
reverse_names_label is reverse_names_result.join("/")
reverse_flags is [true, false]
reverse_flags.reverse()
reverse_flags_label is reverse_flags.join("|")
reverse_label is "Reverse {reverse_values_label} {reverse_result_label} {reverse_names_label} {reverse_flags_label}"
sort_values is [3, 1, 2]
sort_result is sort_values.sort()
sort_values_label is sort_values.join(",")
sort_result_label is sort_result.join(",")
sort_module_values is [9, -1, 4]
sort_module_result is array.sort(sort_module_values)
sort_module_label is sort_module_result.join(":")
sort_label is "Sort {sort_values_label} {sort_result_label} {sort_module_label}"
repeat_values is array.repeat(7, 3)
repeat_values_label is repeat_values.join(",")
repeat_names is array_repeat("ha", 2)
repeat_names_label is repeat_names.join("")
repeat_flags is array.repeat(true, 2)
repeat_flags_label is repeat_flags.join("|")
repeat_empty is array.repeat(9, 0)
repeat_empty_len is repeat_empty.len()
repeat_label is "Repeat {repeat_values_label} {repeat_names_label} {repeat_flags_label} {repeat_empty_len}"
unique_values is array_unique([1, 1, 2, 2, 1])
unique_values_label is unique_values.join(",")
unique_module_values is array.unique([3, 3, 1, 2, 1])
unique_module_label is unique_module_values.join("|")
unique_method_values is [4, 4, 5, 4]
unique_method_result is unique_method_values.unique()
unique_method_label is unique_method_result.join(":")
unique_label is "Unique {unique_values_label} {unique_module_label} {unique_method_label}"
set_union_values is set_union([1, 2, 1], [2, 3], [3, 4])
set_union_values_label is set_union_values.join(",")
set_union_module_values is set.union([5, 5], [6, 5], [7])
set_union_module_label is set_union_module_values.join("|")
set_union_single is set_union([9, 9, 8])
set_union_single_label is set_union_single.join(":")
set_union_label is "SetUnion {set_union_values_label} {set_union_module_label} {set_union_single_label}"
set_intersection_values is set_intersection([1, 2, 2, 3], [2, 3, 4], [3, 2])
set_intersection_values_label is set_intersection_values.join(",")
set_intersection_module_values is set.intersection([5, 6, 5, 7], [4, 5, 7])
set_intersection_module_label is set_intersection_module_values.join("|")
set_difference_values is set_difference([1, 2, 3, 2, 4], [2], [4, 9])
set_difference_values_label is set_difference_values.join(",")
set_difference_module_values is set.difference([5, 6, 5, 7], [5])
set_difference_module_label is set_difference_module_values.join("|")
set_symdiff_values is set_symmetric_difference([1, 2, 3, 3], [3, 4, 2, 5])
set_symdiff_values_label is set_symdiff_values.join(",")
set_symdiff_module_values is set.symmetric_difference([7, 8, 7], [8, 9])
set_symdiff_module_label is set_symdiff_module_values.join("|")
set_symdiff_alias_values is set.symdiff([10, 11], [11, 12])
set_symdiff_alias_label is set_symdiff_alias_values.join(":")
set_subset_true is set_is_subset([1, 2], [0, 1, 2, 3])
set_subset_false is set.is_subset([1, 4], [1, 2, 3])
set_subset_alias is set.subset([2, 2], [2, 3])
set_superset_true is set_is_superset([1, 2, 3], [2, 3])
set_superset_false is set.is_superset([1, 2], [1, 2, 3])
set_superset_alias is set.superset([4, 5], [4])
set_interdiff_label is "SetOps {set_intersection_values_label} {set_intersection_module_label} {set_difference_values_label} {set_difference_module_label} {set_symdiff_values_label} {set_symdiff_module_label} {set_symdiff_alias_label} {set_subset_true} {set_subset_false} {set_subset_alias} {set_superset_true} {set_superset_false} {set_superset_alias}"
clone_source is [8, 9]
clone_result is array.clone(clone_source)
clone_result[0] is 4
clone_copy is array.copy(clone_source)
clone_copy[1] is 5
clone_source_label is clone_source.join(",")
clone_result_label is clone_result.join(",")
clone_copy_label is clone_copy.join(",")
clone_label is "Clone {clone_source_label} {clone_result_label} {clone_copy_label}"
concat_left is [1, 2]
concat_right is [3, 4]
concat_tail is [5]
concat_result is array.concat(concat_left, concat_right, concat_tail)
concat_result[1] is 9
concat_left_label is concat_left.join(",")
concat_right_label is concat_right.join(",")
concat_result_label is concat_result.join(",")
concat_names is array.concat(["A"], ["B", "C"])
concat_names_label is concat_names.join("")
concat_label is "Concat {concat_left_label} {concat_right_label} {concat_result_label} {concat_names_label}"
chunk_values is array.chunk([1, 2, 3, 4, 5], 2)
chunk_count is chunk_values.len()
chunk_first is chunk_values[0]
chunk_second is chunk_values[1]
chunk_last is chunk_values[2]
chunk_first_len is chunk_first.len()
chunk_second_len is chunk_second.len()
chunk_last_len is chunk_last.len()
chunk_first_item is chunk_first[1]
chunk_second_item is chunk_second[0]
chunk_last_item is chunk_last[0]
chunk_empty is array.chunk([], 3)
chunk_empty_len is chunk_empty.len()
chunk_label is "Chunk {chunk_count} {chunk_first_len} {chunk_second_len} {chunk_last_len} {chunk_first_item} {chunk_second_item} {chunk_last_item} {chunk_empty_len}"
zip_values is array.zip([1, 2], [3, 4, 5])
zip_count is zip_values.len()
zip_first is zip_values[0]
zip_second is zip_values[1]
zip_first_len is zip_first.len()
zip_second_len is zip_second.len()
zip_first_left is zip_first[0]
zip_first_right is zip_first[1]
zip_second_left is zip_second[0]
zip_second_right is zip_second[1]
zip_empty is array.zip([], [1])
zip_empty_len is zip_empty.len()
zip_label is "Zip {zip_count} {zip_first_len} {zip_second_len} {zip_first_left} {zip_first_right} {zip_second_left} {zip_second_right} {zip_empty_len}"
flatten_chunks is array.flatten(array.chunk([1, 2, 3, 4, 5], 2))
flatten_chunks_label is flatten_chunks.join(",")
flatten_zip is array_flatten(array.zip([1, 2], [3, 4]))
flatten_zip_label is flatten_zip.join(":")
flatten_method_source is array.chunk([6, 7, 8], 2)
flatten_method_values is flatten_method_source.flatten()
flatten_method_label is flatten_method_values.join("|")
flatten_literal is array.flatten([[9, 10], [11]])
flatten_literal_label is flatten_literal.join("/")
flatten_label is "Flatten {flatten_chunks_label} {flatten_zip_label} {flatten_method_label} {flatten_literal_label}"
dict_api_source is {alpha: 1, beta: 2}
dict_api_keys is dict_api_source.keys()
dict_api_values is dict.values(dict_api_source)
dict_api_direct_keys is dict_keys({gamma: 3, omega: 4})
dict_api_direct_values is dict_values({gamma: 3, omega: 4})
dict_api_items is dict_api_source.items()
dict_api_item_first is dict_api_items[0]
dict_api_item_second is dict.items(dict_api_source)[1]
dict_api_direct_item is dict_items({left: 8, right: 9})[0]
dict_api_items_len is dict_api_items.len()
dict_api_item_first_len is dict_api_item_first.len()
dict_api_item_second_len is dict_api_item_second.len()
dict_api_direct_item_len is dict_api_direct_item.len()
dict_api_item_first_value is dict_api_item_first[1]
dict_api_item_second_value is dict_api_item_second[1]
dict_api_direct_item_value is dict_api_direct_item[1]
dict_api_keys_label is dict_api_keys.join("|")
dict_api_values_label is dict_api_values.join(":")
dict_api_direct_keys_label is dict_api_direct_keys.join("/")
dict_api_direct_values_label is dict_api_direct_values.join(",")
dict_api_label is "DictApi {dict_api_keys_label} {dict_api_values_label} {dict_api_direct_keys_label} {dict_api_direct_values_label} {dict_api_items_len} {dict_api_item_first_len} {dict_api_item_second_len} {dict_api_direct_item_len} {dict_api_item_first_value} {dict_api_item_second_value} {dict_api_direct_item_value}"
dict_api_pick_source is {alpha: 1, beta: 2, gamma: 3}
dict_api_picked is dict.pick(dict_api_pick_source, ["gamma", "alpha", "missing", "gamma"])
dict_api_picked_method is dict_api_pick_source.pick(["beta", "missing"])
dict_api_direct_pick is dict_pick({left: 8, right: 9}, ["right"])
dict_api_picked_keys is dict_api_picked.keys()
dict_api_picked_values is dict_api_picked.values()
dict_api_picked_keys_label is dict_api_picked_keys.join("|")
dict_api_picked_values_label is dict_api_picked_values.join(":")
dict_api_picked_gamma is dict_api_picked.gamma
dict_api_picked_alpha is dict_api_picked.alpha
dict_api_picked_method_beta is dict_api_picked_method.beta
dict_api_direct_pick_right is dict_api_direct_pick.right
dict_api_pick_label is "DictPick {dict_api_picked_keys_label} {dict_api_picked_values_label} {dict_api_picked_gamma} {dict_api_picked_alpha} {dict_api_picked_method_beta} {dict_api_direct_pick_right}"
dict_api_omit_source is {alpha: 1, beta: 2, gamma: 3}
dict_api_omitted is dict.omit(dict_api_omit_source, ["beta", "missing", "beta"])
dict_api_omitted_method is dict_api_omit_source.omit(["alpha"])
dict_api_direct_omit is dict_omit({left: 8, right: 9}, ["left"])
dict_api_omitted_keys is dict_api_omitted.keys()
dict_api_omitted_values is dict_api_omitted.values()
dict_api_omitted_keys_label is dict_api_omitted_keys.join("|")
dict_api_omitted_values_label is dict_api_omitted_values.join(":")
dict_api_omitted_alpha is dict_api_omitted.alpha
dict_api_omitted_gamma is dict_api_omitted.gamma
dict_api_omitted_method_beta is dict_api_omitted_method.beta
dict_api_direct_omit_right is dict_api_direct_omit.right
dict_api_omit_label is "DictOmit {dict_api_omitted_keys_label} {dict_api_omitted_values_label} {dict_api_omitted_alpha} {dict_api_omitted_gamma} {dict_api_omitted_method_beta} {dict_api_direct_omit_right}"
dict_api_merge_left is {alpha: 1, beta: 2}
dict_api_merged is dict.merge(dict_api_merge_left, {beta: 20, gamma: 3}, {gamma: 30, delta: 4})
dict_api_merged_method is dict_api_merge_left.merge({beta: 22, omega: 5})
dict_api_direct_merge is dict_merge({left: 8}, {left: 80, right: 9})
dict_api_merged_keys is dict_api_merged.keys()
dict_api_merged_values is dict_api_merged.values()
dict_api_merged_keys_label is dict_api_merged_keys.join("|")
dict_api_merged_values_label is dict_api_merged_values.join(":")
dict_api_merged_alpha is dict_api_merged.alpha
dict_api_merged_beta is dict_api_merged.beta
dict_api_merged_gamma is dict_api_merged.gamma
dict_api_merged_delta is dict_api_merged.delta
dict_api_merged_method_beta is dict_api_merged_method.beta
dict_api_merged_method_omega is dict_api_merged_method.omega
dict_api_direct_merge_left is dict_api_direct_merge.left
dict_api_direct_merge_right is dict_api_direct_merge.right
dict_api_merge_label is "DictMerge {dict_api_merged_keys_label} {dict_api_merged_values_label} {dict_api_merged_alpha} {dict_api_merged_beta} {dict_api_merged_gamma} {dict_api_merged_delta} {dict_api_merged_method_beta} {dict_api_merged_method_omega} {dict_api_direct_merge_left} {dict_api_direct_merge_right}"
dict_path_source is {player: {stats: {hp: 42, name: "Ari"}}}
dict_path_hp is dict_get_path(dict_path_source, "player.stats.hp")
dict_path_name is dict.get_path(dict_path_source, "player.stats.name")
dict_path_name_method is dict_path_source.get_path("player.stats.name")
dict_path_missing is dict_get_path(dict_path_source, "player.stats.mp")
dict_path_label is "DictPath {dict_path_hp} {dict_path_name} {dict_path_name_method} {dict_path_missing}"
json_path_hp is json_path(dict_path_source, "player.stats.hp")
json_path_name is json.path(dict_path_source, "player.stats.name")
json_path_name_method is json.get_path(dict_path_source, "player.stats.name")
json_has_hp is json_has_path(dict_path_source, "player.stats.hp")
json_has_missing is json.has_path(dict_path_source, "player.stats.mp")
json_path_label is "JsonPath {json_path_hp} {json_path_name} {json_path_name_method} {json_has_hp} {json_has_missing}"
clear_values is [7, 8]
clear_result is clear_values.clear()
clear_len is clear_values.len()
clear_label is "Clear {clear_result} {clear_len}"
insert_values is [1, 3]
insert_values.insert(1, 2)
insert_label is insert_values.join(",")
insert_front is [2, 3]
insert_front.insert(-9, 1)
insert_front_label is insert_front.join(",")
insert_tail is [1, 2]
insert_tail.insert(99, 3)
insert_tail_label is insert_tail.join(",")
remove_values is [4, 5, 6]
removed_mid is remove_values.remove(1)
remove_after_mid_label is remove_values.join(":")
removed_tail is remove_values.remove(-1)
remove_after_tail_label is remove_values.join(":")
removed_missing is remove_values.remove(9)
remove_missing_len is remove_values.len()
remove_label is "InsertRemove {insert_label} {insert_front_label} {insert_tail_label} {removed_mid} {remove_after_mid_label} {removed_tail} {remove_after_tail_label} {removed_missing} {remove_missing_len}"
join_names is ["Ada", "Grace"]
join_scores is [1, 2, 3]
join_flags is [true, false]
join_nils is [nil, nil]
join_names_label is join_names.join("|")
join_scores_label is join_scores.join(",")
join_flags_label is join_flags.join("/")
join_nils_label is join_nils.join("-")
join_label is "Join {join_names_label} {join_scores_label} {join_flags_label} {join_nils_label}"
slice_scores is join_scores.slice(1)
slice_names is join_names.slice(0, 1)
slice_flags is array.slice(join_flags, -2, 1)
slice_nils is join_nils.slice(1, 5)
slice_scores_label is slice_scores.join(":")
slice_names_label is slice_names.join("/")
slice_flags_label is slice_flags.join("|")
slice_nils_label is slice_nils.join("/")
slice_label is "Slice {slice_scores_label} {slice_names_label} {slice_flags_label} {slice_nils_label}"
dynamic_idx is 1
mixed_to_str_idx is 2
mixed_score_idx is 0
mixed_type_idx is 2
mixed_bool_idx is 2
mixed_len_idx is 1
mixed_profile_name_lookup is "name"
mixed_profile_active_lookup is "active"
mixed_profile_missing_lookup is "missing"
mixed_profile_score_lookup is "score"
profile_key_lookup is "city"
profile_update_key is "city"
empty_values is []
empty_profile is {}
nested is {items: [1], empty_items: [], profile: {ready: 1}, empty_profile: {}}
nested_arrays is [[1], []]
nested_profiles is [{ready: 1}, {}]
point is new AstPoint(6, 7)
child is new AstChild(2, 5)
forward_box is new AstForwardLabelBox("Ari")
forward_child is new AstForwardChild()
super_object_param_type_label is forward_child.child_label_type(forward_box)
forward_bag is new AstForwardBag([1, 2])
forward_array_child is new AstForwardArrayChild()
super_object_param_array_len_label is forward_array_child.child_values_len(forward_bag)
tagged is new AstTagged()
tagged_objects is [new AstTagged(), new AstTagged()]
tagged_map is {primary: new AstTagged(), secondary: new AstTagged()}
param_tagged is new AstParamTagged("agent", true, nil)
tagged_box is new AstTaggedBox(new AstTagged())
tagged_box_child is new AstTaggedBoxChild(new AstTagged())
collection_tagged is new AstCollectionTagged(values, meta)
super_collection_tagged is new AstCollectionChild(values, meta)
field_conflict_num is new AstFieldConflict(4)
field_conflict_text is new AstFieldConflict("bad")
recursive_counter is new AstRecursive(4)
function_holder is new AstFunctionHolder(block_double_ast)
function_holder_alt is new AstFunctionHolder(triple_ast)
function_child is new AstFunctionChild(block_double_ast)
binary_function_holder is new AstBinaryFunctionHolder(add_pair_ast)
binary_function_holder_alt is new AstBinaryFunctionHolder(mul_pair_ast)
binary_function_child is new AstBinaryFunctionChild(add_pair_ast)
method_thrower is new AstMethodThrower()
super_method_thrower is new AstSuperMethodThrower()
super_echo_source is "local-super"
score is add_ast(meta.score, AstWasmMode.READY)
mixed_values is [score, profile.name, flags.active, flags.missing]
mixed_values_from_func is make_mixed_values_ast()
mixed_profile is {score: score, name: profile.name, active: flags.active, missing: flags.missing}
if_runtime_label is ""
if mixed_profile[mixed_profile_active_lookup] then
  if_runtime_label is "IfRuntime then {type([1, \"x\"])} {length([1, \"x\"])} {to_bool({ok: true})} {type(return_direct_function())} {to_bool(return_direct_function())} {to_str(return_direct_function())}"
else
  if_runtime_label is "IfRuntime bad"
end
if_else_runtime_label is ""
if mixed_profile[mixed_profile_missing_lookup] then
  if_else_runtime_label is "IfRuntime bad"
else
  if_else_runtime_label is "IfRuntime else {type([1, \"x\"])} {length([1, \"x\"])} {to_bool({ok: true})} {type(return_direct_function())} {to_bool(return_direct_function())} {to_str(return_direct_function())}"
end
match_runtime_label is ""
match mixed_profile[mixed_profile_active_lookup]
when false then match_runtime_label is "MatchRuntime bad"
when true then match_runtime_label is "MatchRuntime bool {type([1, \"x\"])} {length([1, \"x\"])} {to_bool({ok: true})} {type(return_direct_function())} {to_bool(return_direct_function())} {to_str(return_direct_function())}"
when _ then match_runtime_label is "MatchRuntime miss"
end
match_nil_runtime_label is ""
match mixed_profile[mixed_profile_missing_lookup]
when true then match_nil_runtime_label is "MatchRuntime bad"
when nil then match_nil_runtime_label is "MatchRuntime nil {type([1, \"x\"])} {length([1, \"x\"])} {to_bool({ok: true})} {type(return_direct_function())} {to_bool(return_direct_function())} {to_str(return_direct_function())}"
when _ then match_nil_runtime_label is "MatchRuntime miss"
end
repeat_conflict_probe is 0
repeat 0 do
  repeat_conflict_probe is "skip"
end
repeat_conflict_after is repeat_conflict_probe + 1
repeat_conflict_profile is {tag: 0}
repeat 0 do
  repeat_conflict_profile.tag is "skip"
end
repeat_conflict_field_after is repeat_conflict_profile.tag + 1
while_conflict_probe is 0
while false do
  while_conflict_probe is "skip"
end
while_conflict_after is while_conflict_probe + 1
while_conflict_profile is {tag: 0}
while false do
  while_conflict_profile.tag is "skip"
end
while_conflict_field_after is while_conflict_profile.tag + 1
while_dynamic_limits is [2]
while_runtime_counter is 0
while_runtime_label is ""
while while_runtime_counter < while_dynamic_limits[0] do
  while_runtime_counter += 1
  while_runtime_label is "WhileRuntime {while_runtime_counter} {type([1, \"x\"])} {length([1, \"x\"])} {to_bool({ok: true})} {type(return_direct_function())} {to_bool(return_direct_function())} {to_str(return_direct_function())}"
end
for_conflict_probe is 0
for loop_conflict_i in 2 to 1 do
  for_conflict_probe is "skip"
end
for_conflict_after is for_conflict_probe + 1
for_conflict_profile is {tag: 0}
for loop_conflict_field_i in 2 to 1 do
  for_conflict_profile.tag is "skip"
end
for_conflict_field_after is for_conflict_profile.tag + 1
for_dynamic_froms is [1]
for_dynamic_tos is [2]
for_dynamic_steps is [1]
for_runtime_label is ""
for for_runtime_i in for_dynamic_froms[0] to for_dynamic_tos[0] step for_dynamic_steps[0] do
  for_runtime_label is "ForRuntime {for_runtime_i} {type([1, \"x\"])} {length([1, \"x\"])} {to_bool({ok: true})} {type(return_direct_function())} {to_bool(return_direct_function())} {to_str(return_direct_function())}"
end
foreach_array_conflict_probe is 0
empty_loop_items is []
for empty_item in empty_loop_items do
  foreach_array_conflict_probe is "skip"
end
foreach_array_conflict_after is foreach_array_conflict_probe + 1
foreach_array_conflict_profile is {tag: 0}
for empty_field_item in empty_loop_items do
  foreach_array_conflict_profile.tag is "skip"
end
foreach_array_conflict_field_after is foreach_array_conflict_profile.tag + 1
foreach_dict_conflict_probe is 0
empty_loop_profile is {}
for empty_key, empty_value in empty_loop_profile do
  foreach_dict_conflict_probe is "skip"
end
foreach_dict_conflict_after is foreach_dict_conflict_probe + 1
foreach_dict_conflict_profile is {tag: 0}
for empty_field_key, empty_field_value in empty_loop_profile do
  foreach_dict_conflict_profile.tag is "skip"
end
foreach_dict_conflict_field_after is foreach_dict_conflict_profile.tag + 1
coalesce_effect_count is 0
func coalesce_rhs_ast() do
  global coalesce_effect_count
  coalesce_effect_count += 1
  return "called"
end
label is "score {score}"
greeting is "Hello {profile.name}"
nested_function_result is nested_label_pair("call ", "ok")
nested_function_type is type(nested_label_pair("type ", "ok"))
nested_method_result is nested_method_label("method-ok")
nested_method_type is type(nested_method_label("method-type"))
if_local_method_result is if_local_method_label(true, "if-ok")
if_local_method_type is type(if_local_method_label(false, "if-type"))
match_local_method_result is match_local_method_label(1, "match-ok")
match_local_method_type is type(match_local_method_label(2, "match-type"))
score_label is "Score {meta.score}"
nil_profile is nil
optional_dot_nil_value is nil_profile?.name
optional_nested_nil_value is nil_profile?.profile?.name
optional_existing_value is profile?.name
nil_index_source is nil
nil_numeric_index_value is nil_index_source[0]
nil_string_index_value is nil_index_source["name"]
optional_coalesce_nil_value is nil_profile?.name ?? "anon"
optional_coalesce_nested_value is nil_profile?.profile?.name ?? "anon"
optional_coalesce_existing_value is profile?.name ?? "anon"
optional_coalesce_interp_label is "Maybe {nil_profile?.name ?? \"anon\"} Existing {profile?.name ?? \"anon\"}"
optional_coalesce_call_label is "CoalesceCalls {type(nil_profile?.name ?? \"anon\")} {length(nil_profile?.name ?? \"anon\")} {to_bool(nil_profile?.name ?? \"anon\")} {to_str(nil_profile?.name ?? \"anon\")}"
collection_holder is {items: ["Ada", "Grace"]}
optional_items_value is nil_profile?.items ?? ["fallback", "items"]
optional_existing_items_value is collection_holder?.items ?? ["fallback"]
optional_items_label is "Items {to_str(optional_items_value)} {to_str(optional_existing_items_value)} {length(optional_items_value)} {to_bool(optional_items_value)}"
falsey_holder is {active: false, count: 0}
optional_false_value is falsey_holder?.active ?? true
optional_zero_value is falsey_holder?.count ?? 99
optional_false_fallback_value is nil_profile?.active ?? true
optional_zero_fallback_value is nil_profile?.count ?? 99
optional_falsey_label is "Falsey {optional_false_value} {optional_zero_value} {optional_false_fallback_value} {optional_zero_fallback_value}"
direct_falsey_label is "DirectFalsey {false ?? true} {0 ?? 99} {nil ?? \"fallback\"}"
direct_falsey_call_label is "DirectCalls {to_bool(false ?? true)} {to_str(0 ?? 99)} {type(nil ?? \"fallback\")}"
mixed_coalesce_label is "MixedCoalesce {type(1 ?? \"fallback\")} {to_str(1 ?? \"fallback\")} {type(nil ?? [\"fallback\"])} {to_str(nil ?? [\"fallback\"])}"
coalesce_short_value is "left" ?? coalesce_rhs_ast()
coalesce_effect_after_short is coalesce_effect_count
coalesce_nil_value is nil ?? coalesce_rhs_ast()
coalesce_effect_after_nil is coalesce_effect_count
coalesce_short_label is "ShortCircuit {coalesce_short_value} {coalesce_effect_after_short} {coalesce_nil_value} {coalesce_effect_after_nil}"
flag_label is "Active {flags.active} Missing {flags.missing}"
array_label is "Name {names[0]} Active {states[0]} Missing {states[2]}"
dynamic_name_label is "Dynamic {names[dynamic_idx]}"
dynamic_bool_label is "Bool {bools[dynamic_idx]}"
search_text is "sura wasm"
ast_string_search_label is "Search {contains(search_text, \"wasm\")} {startsWith(search_text, \"su\")} {endsWith(search_text, \"asm\")} {string.contains(search_text, \"python\")}"
ast_string_index_of_label is "Index {indexOf(search_text, \"wasm\")} {indexOf(search_text, \"python\")} {string.index_of(search_text, \"sura\")} {search_text.index_of(\"asm\")}"
transform_text is "  Sura WASM  "
ast_string_transform_label is "Transform {upper(search_text)} {lower(\"SURA WASM\")} {trim(transform_text)} {string.upper(\"ok\")} {search_text.upper()} {\"SURA\".lower()} {\"  core  \".trim()}"
ast_string_replace_label is "Replace {replace(search_text, \"wasm\", \"WASM\")} {replace(search_text, \"\", \"x\")} {string.replace(\"sura wasm sura\", \"sura\", \"SURA\")} {\"banana\".replace(\"na\", \"NA\")}"
ast_string_slice_label is "Slice {substring(search_text, 5)} {slice(search_text, -4)} {string.substring(search_text, 0, 4)} {string.sub(search_text, 5)} {search_text.sub(0, 4)} {search_text.slice(5)} {search_text.substring(5, 9)}"
ast_string_repeat_label is "Repeat {string_repeat(\"ha\", 3)} {string.repeat(\"go\", 2)} {\"na\".repeat(4)} {string.repeat(\"z\", 0)}"
ast_string_pad_label is "Pad {string_pad_left(\"7\", 3, \"0\")} {string.pad_right(\"go\", 4, \"!\")} {\"x\".pad_left(3, \"_\")} {\"ok\".pad_right(2, \"?\")}"
split_parts is "sura,wasm,lang".split(",")
split_module_parts is string.split("red::blue::green", "::")
split_empty_parts is "solo".split("")
ast_string_split_label is "Split {split_parts.len()} {split_parts[0]} {split_parts[1]} {split_parts.join(\"/\")} {split_module_parts[2]} {split_empty_parts.len()} {split_empty_parts[0]}"
line_parts is string_lines("top\nmid\nend")
line_module_parts is string.lines("red\ngreen")
line_receiver_text is "left\nright\n"
line_receiver_parts is line_receiver_text.lines()
empty_line_parts is string.lines("")
ast_string_lines_label is "Lines {line_parts.len()} {line_parts[0]} {line_parts[2]} {line_parts.join(\"|\")} {line_module_parts[1]} {line_receiver_parts.len()} {line_receiver_parts.join(\"/\")} {empty_line_parts.len()}"
word_parts is string_words("  alpha beta  gamma  ")
word_module_parts is string.words("red green")
word_receiver_text is "left  right\ncenter"
word_receiver_parts is word_receiver_text.words()
empty_word_parts is string.words("     ")
ast_string_words_label is "Words {word_parts.len()} {word_parts[0]} {word_parts[2]} {word_parts.join(\"|\")} {word_module_parts[1]} {word_receiver_parts.len()} {word_receiver_parts.join(\"/\")} {empty_word_parts.len()}"
chunk_parts is text_chunks("abcdefg", 3, 1)
chunk_alias_parts is text_chunk("tiny")
chunk_module_parts is string.chunks("hello", 2)
chunk_receiver_text is "sura"
chunk_receiver_parts is chunk_receiver_text.chunks(2, 0)
empty_chunk_parts is string.chunks("", 2)
ast_text_chunks_label is "Chunks {chunk_parts.len()} {chunk_parts[0]} {chunk_parts[1]} {chunk_parts[2]} {chunk_parts.join(\"|\")} {chunk_alias_parts.len()} {chunk_alias_parts[0]} {chunk_module_parts.join(\"/\")} {chunk_receiver_parts.join(\"-\")} {empty_chunk_parts.len()}"
profile_dynamic_label is "Profile {profile[profile_key_lookup]}"
profile_has_name is profile.has("name")
profile_has_missing is profile.has("missing")
profile_contains_city is profile.contains("city")
dict_module_has_name is dict.has(profile, "name")
dict_module_contains_missing is dict.contains(profile, "missing")
dynamic_has_profile is score > 0 ? {name: "Ada", missing: nil} : [1]
dynamic_profile_has_missing is dynamic_has_profile.has("missing")
dynamic_profile_contains_name is dynamic_has_profile.contains("name")
dict_has_label is "DictHas {profile_has_name} {profile_has_missing} {profile_contains_city} {dict_module_has_name} {dict_module_contains_missing} {dynamic_profile_has_missing} {dynamic_profile_contains_name}"
collection_profile_key is "bonus"
type_label is "Types {type(profile)} {type(names)} {type(flags.active)} {type(flags.missing)} {type(score)} {type(point)}"
function_alias is block_double_ast
function_type_label is "Functions {type(block_double_ast)} {type(function_alias)} {to_bool(block_double_ast)}"
function_to_str_label is "Fn {to_str(block_double_ast)}"
function_alias_to_str_label is "Alias {to_str(function_alias)}"
function_handlers is [block_double_ast, format_pair_ast]
function_map is {primary: block_double_ast, secondary: format_pair_ast}
function_idx is 1
function_key_lookup is "primary"
function_update_key is "secondary"
tagged_key_lookup is "secondary"
dynamic_tagged_update_key is "current"
function_choice is true ? block_double_ast : format_pair_ast
function_pick_call is pick_function_ast(false)
function_pick_true is pick_function_ast(true)
function_pick_true_call is function_pick_true(16)
function_match_pick is pick_function_match_ast(1)
function_match_pick_call is function_match_pick(17)
function_passed is pass_function_ast(block_double_ast)
function_param_call is call_function_param_ast(block_double_ast, 12)
function_param_call_label is describe_call_function_param_ast(block_double_ast, 13)
dynamic_function_param_call_a is call_dynamic_function_param_ast(block_double_ast, 6)
dynamic_function_param_call_b is call_dynamic_function_param_ast(triple_ast, 6)
dynamic_local_function_call_a is call_dynamic_local_function_ast(true, 5)
dynamic_local_function_call_b is call_dynamic_local_function_ast(false, 5)
dynamic_zero_function_call_a is call_dynamic_zero_function_ast(true)
dynamic_zero_function_call_b is call_dynamic_zero_function_ast(false)
dynamic_zero_function_call_label is "DynamicZeroFunction {call_dynamic_zero_function_ast(true)} {call_dynamic_zero_function_ast(false)}"
dynamic_binary_function_call_a is call_dynamic_binary_function_ast(true, 3, 4)
dynamic_binary_function_call_b is call_dynamic_binary_function_ast(false, 3, 4)
dynamic_binary_function_call_label is "DynamicBinaryFunction {call_dynamic_binary_function_ast(true, 3, 4)} {call_dynamic_binary_function_ast(false, 3, 4)}"
dynamic_binary_dict_function_call_a is call_dynamic_binary_dict_function_ast(true, 5, 6)
dynamic_binary_dict_function_call_b is call_dynamic_binary_dict_function_ast(false, 5, 6)
dynamic_binary_dict_function_call_label is "DynamicBinaryDictFunction {call_dynamic_binary_dict_function_ast(true, 5, 6)} {call_dynamic_binary_dict_function_ast(false, 5, 6)}"
dynamic_binary_array_function_call_a is call_dynamic_binary_array_function_ast(true, 7, 8)
dynamic_binary_array_function_call_b is call_dynamic_binary_array_function_ast(false, 7, 8)
dynamic_binary_array_function_call_label is "DynamicBinaryArrayFunction {call_dynamic_binary_array_function_ast(true, 7, 8)} {call_dynamic_binary_array_function_ast(false, 7, 8)}"
foreach_binary_function_call_a is call_foreach_binary_function_ast(true, 2, 9)
foreach_binary_function_call_b is call_foreach_binary_function_ast(false, 2, 9)
foreach_binary_function_call_label is "ForeachBinaryFunction {call_foreach_binary_function_ast(true, 2, 9)} {call_foreach_binary_function_ast(false, 2, 9)}"
dynamic_binary_param_function_call_a is call_dynamic_binary_param_function_ast(add_pair_ast, 9, 10)
dynamic_binary_param_function_call_b is call_dynamic_binary_param_function_ast(mul_pair_ast, 9, 10)
dynamic_binary_param_function_call_label is "DynamicBinaryParamFunction {call_dynamic_binary_param_function_ast(add_pair_ast, 9, 10)} {call_dynamic_binary_param_function_ast(mul_pair_ast, 9, 10)}"
dynamic_triple_function_call_a is call_dynamic_triple_function_ast(true, 2, 3, 4)
dynamic_triple_function_call_b is call_dynamic_triple_function_ast(false, 2, 3, 4)
dynamic_triple_function_call_label is "DynamicTripleFunction {call_dynamic_triple_function_ast(true, 2, 3, 4)} {call_dynamic_triple_function_ast(false, 2, 3, 4)}"
dynamic_quad_function_call_a is call_dynamic_quad_function_ast(true, 1, 2, 3, 4)
dynamic_quad_function_call_b is call_dynamic_quad_function_ast(false, 1, 2, 3, 4)
dynamic_quad_function_call_label is "DynamicQuadFunction {call_dynamic_quad_function_ast(true, 1, 2, 3, 4)} {call_dynamic_quad_function_ast(false, 1, 2, 3, 4)}"
dynamic_five_function_call_a is call_dynamic_five_function_ast(true, 1, 2, 3, 4, 5)
dynamic_five_function_call_b is call_dynamic_five_function_ast(false, 1, 2, 3, 4, 5)
dynamic_five_function_call_label is "DynamicFiveFunction {call_dynamic_five_function_ast(true, 1, 2, 3, 4, 5)} {call_dynamic_five_function_ast(false, 1, 2, 3, 4, 5)}"
dynamic_eight_function_call_a is call_dynamic_eight_function_ast(true, 1, 2, 3, 4, 5, 6, 7, 8)
dynamic_eight_function_call_b is call_dynamic_eight_function_ast(false, 1, 2, 3, 4, 5, 6, 7, 8)
dynamic_eight_function_call_label is "DynamicEightFunction {call_dynamic_eight_function_ast(true, 1, 2, 3, 4, 5, 6, 7, 8)} {call_dynamic_eight_function_ast(false, 1, 2, 3, 4, 5, 6, 7, 8)}"
dynamic_string_function_call_a is call_dynamic_string_function_ast(true, "x")
dynamic_string_function_call_b is call_dynamic_string_function_ast(false, "x")
dynamic_string_function_call_label is "DynamicStringFunction {call_dynamic_string_function_ast(true, \"x\")} {call_dynamic_string_function_ast(false, \"x\")}"
dynamic_bool_function_call_a is call_dynamic_bool_function_ast(true, 5)
dynamic_bool_function_call_b is call_dynamic_bool_function_ast(false, 5)
dynamic_bool_function_call_label is "DynamicBoolFunction {call_dynamic_bool_function_ast(true, 5)} {call_dynamic_bool_function_ast(false, 5)} {to_bool(call_dynamic_bool_function_ast(false, 5))}"
dynamic_array_function_call_a is call_dynamic_array_function_ast(true, 7)
dynamic_array_function_call_b is call_dynamic_array_function_ast(false, 7)
dynamic_array_function_call_label is "DynamicArrayFunction {length(call_dynamic_array_function_ast(true, 7))} {to_str(call_dynamic_array_function_ast(false, 7))}"
dynamic_nil_function_call_a is call_dynamic_nil_function_ast(true, 0)
dynamic_nil_function_call_label is "DynamicNilFunction {type(call_dynamic_nil_function_ast(false, 0))} {to_bool(call_dynamic_nil_function_ast(false, 0))}"
dynamic_dict_return_function_call_a is call_dynamic_dict_return_function_ast(true, 7)
dynamic_dict_return_function_call_b is call_dynamic_dict_return_function_ast(false, 7)
dynamic_dict_return_function_call_label is "DynamicDictReturnFunction {length(call_dynamic_dict_return_function_ast(true, 7))} {to_str(call_dynamic_dict_return_function_ast(false, 7).name)}"
dynamic_returned_function_call is call_dynamic_function_return_function_ast(false, 0)
dynamic_function_return_function_call is dynamic_returned_function_call(3, 4)
dynamic_function_return_function_call_label is "DynamicFunctionReturnFunction {type(dynamic_returned_function_call)} {to_bool(dynamic_returned_function_call)} {dynamic_returned_function_call(3, 4)}"
dynamic_dict_lookup_dict_return_function_a is call_dynamic_dict_lookup_dict_return_function_ast(true, 7)
dynamic_dict_lookup_dict_return_function_b is call_dynamic_dict_lookup_dict_return_function_ast(false, 7)
dynamic_dict_lookup_dict_return_function_label is "DynamicDictLookupDictReturnFunction {length(call_dynamic_dict_lookup_dict_return_function_ast(true, 7))} {call_dynamic_dict_lookup_dict_return_function_ast(false, 7).score} {to_str(call_dynamic_dict_lookup_dict_return_function_ast(false, 7).name)}"
dynamic_array_lookup_returned_function is call_dynamic_array_lookup_function_return_function_ast(false, 0)
dynamic_array_lookup_function_return_call is dynamic_array_lookup_returned_function(3, 4)
dynamic_array_lookup_function_return_label is "DynamicArrayLookupFunctionReturn {type(dynamic_array_lookup_returned_function)} {to_bool(dynamic_array_lookup_returned_function)} {dynamic_array_lookup_returned_function(3, 4)}"
dynamic_dict_function_call_a is call_dynamic_dict_function_key_ast(true, 6)
dynamic_dict_function_call_b is call_dynamic_dict_function_key_ast(false, 6)
direct_dynamic_dict_function_call_label is "DirectDynamicDictFn {call_dynamic_dict_function_key_ast(false, 7)}"
local_function_value_label is local_function_value_ast()
inline_function_value_label is inline_function_expr_values_ast()
direct_inline_function_value_label is direct_inline_function_expr_values_ast()
returned_inline_function_ast is returned_inline_function_value_ast()
returned_inline_function_call is returned_inline_function_ast(18)
returned_inline_function_label is "ReturnInlineFn {type(returned_inline_function_ast)} {to_bool(returned_inline_function_ast)} {to_str(returned_inline_function_ast)} {returned_inline_function_call}"
captured_inline_function_call is captured_inline_function_value_ast()
captured_inline_function_param_call is captured_inline_function_param_ast(7)
captured_inline_function_alias_call is captured_inline_function_alias_ast()
captured_inline_function_text_call is captured_inline_function_text_ast()
captured_inline_function_bool_call is captured_inline_function_bool_ast()
captured_inline_function_if_merge_call is captured_inline_function_if_merge_ast(true)
captured_inline_function_match_merge_call is captured_inline_function_match_merge_ast(1)
captured_inline_function_collection_call is captured_inline_function_collection_ast()
returned_captured_inline_function is returned_captured_inline_function_ast()
returned_captured_inline_function_call is returned_captured_inline_function(7)
returned_param_captured_inline_function is returned_param_captured_inline_function_ast(7)
returned_param_captured_inline_function_call is returned_param_captured_inline_function(5)
returned_param_function_capture is returned_param_function_capture_ast(block_double_ast)
returned_param_function_capture_call is returned_param_function_capture(9)
returned_param_function_capture_label is "ReturnedFunctionCapture {type(returned_param_function_capture)} {to_bool(returned_param_function_capture)} {returned_param_function_capture_call}"
returned_param_alias_captured_inline_function is returned_param_alias_captured_inline_function_ast(7)
returned_param_alias_captured_inline_function_call is returned_param_alias_captured_inline_function(5)
returned_param_if_alias_captured_inline_function is returned_param_if_alias_captured_inline_function_ast(true, 7)
returned_param_if_alias_captured_inline_function_call is returned_param_if_alias_captured_inline_function(5)
returned_param_if_false_no_else_captured_inline_function is returned_param_if_false_no_else_captured_inline_function_ast(false, 7)
returned_param_if_false_no_else_captured_inline_function_call is returned_param_if_false_no_else_captured_inline_function(5)
returned_param_if_unknown_no_else_captured_inline_function is returned_param_if_unknown_no_else_captured_inline_function_ast(7)
returned_param_if_unknown_no_else_captured_inline_function_call is returned_param_if_unknown_no_else_captured_inline_function(5)
returned_param_match_alias_captured_inline_function is returned_param_match_alias_captured_inline_function_ast(1, 7)
returned_param_match_alias_captured_inline_function_call is returned_param_match_alias_captured_inline_function(5)
returned_param_match_no_arm_captured_inline_function is returned_param_match_no_arm_captured_inline_function_ast(2, 7)
returned_param_match_no_arm_captured_inline_function_call is returned_param_match_no_arm_captured_inline_function(5)
returned_param_repeat_alias_captured_inline_function is returned_param_repeat_alias_captured_inline_function_ast(7)
returned_param_repeat_alias_captured_inline_function_call is returned_param_repeat_alias_captured_inline_function(5)
returned_param_repeat_accum_captured_inline_function is returned_param_repeat_accum_captured_inline_function_ast(7)
returned_param_repeat_accum_captured_inline_function_call is returned_param_repeat_accum_captured_inline_function(5)
returned_param_for_accum_captured_inline_function is returned_param_for_accum_captured_inline_function_ast(7)
returned_param_for_accum_captured_inline_function_call is returned_param_for_accum_captured_inline_function(5)
returned_param_for_zero_captured_inline_function is returned_param_for_zero_captured_inline_function_ast(7)
returned_param_for_zero_captured_inline_function_call is returned_param_for_zero_captured_inline_function(5)
returned_param_foreach_accum_captured_inline_function is returned_param_foreach_accum_captured_inline_function_ast(7)
returned_param_foreach_accum_captured_inline_function_call is returned_param_foreach_accum_captured_inline_function(5)
returned_param_foreach_empty_captured_inline_function is returned_param_foreach_empty_captured_inline_function_ast(7)
returned_param_foreach_empty_captured_inline_function_call is returned_param_foreach_empty_captured_inline_function(5)
returned_param_foreach_empty_dict_captured_inline_function is returned_param_foreach_empty_dict_captured_inline_function_ast(7)
returned_param_foreach_empty_dict_captured_inline_function_call is returned_param_foreach_empty_dict_captured_inline_function(5)
returned_param_while_false_captured_inline_function is returned_param_while_false_captured_inline_function_ast(false, 7)
returned_param_while_false_captured_inline_function_call is returned_param_while_false_captured_inline_function(5)
returned_param_while_literal_false_captured_inline_function is returned_param_while_literal_false_captured_inline_function_ast(7)
returned_param_while_literal_false_captured_inline_function_call is returned_param_while_literal_false_captured_inline_function(5)
captured_inline_function_label is "CapturedInlineFn {captured_inline_function_call} {captured_inline_function_param_call} {captured_inline_function_alias_call} {captured_inline_function_text_call} {captured_inline_function_bool_call} {captured_inline_function_if_merge_call} {captured_inline_function_match_merge_call} {captured_inline_function_collection_call} {type(returned_captured_inline_function)} {to_str(returned_captured_inline_function)} {returned_captured_inline_function_call} {returned_param_captured_inline_function_call} {returned_param_alias_captured_inline_function_call} {returned_param_if_alias_captured_inline_function_call} {returned_param_if_false_no_else_captured_inline_function_call} {returned_param_if_unknown_no_else_captured_inline_function_call} {returned_param_match_alias_captured_inline_function_call} {returned_param_match_no_arm_captured_inline_function_call} {returned_param_repeat_alias_captured_inline_function_call} {returned_param_repeat_accum_captured_inline_function_call} {returned_param_for_accum_captured_inline_function_call} {returned_param_for_zero_captured_inline_function_call} {returned_param_foreach_accum_captured_inline_function_call} {returned_param_foreach_empty_captured_inline_function_call} {returned_param_foreach_empty_dict_captured_inline_function_call} {returned_param_while_false_captured_inline_function_call} {returned_param_while_literal_false_captured_inline_function_call}"
control_flow_function_alias_label is "FlowFn {control_flow_function_alias_ast(true)} {loop_function_alias_ast()} {match_function_alias_ast(1)}"
function_lookup_label is "Function lookup {type(function_handlers[function_idx])} {type(function_map[function_key_lookup])} {to_bool(function_map[function_key_lookup])}"
function_index_to_str_label is "FnIndex {to_str(function_handlers[0])} {to_str(function_map[\"primary\"])}"
function_dynamic_to_str_label is "FnDynamic {to_str(function_map[function_key_lookup])}"
function_dynamic_index_to_str_label is "FnDynIndex {to_str(function_handlers[function_idx])}"
function_param_label is describe_function_ast(block_double_ast)
function_holder_label is function_holder.handler_label()
function_holder_call is function_holder.call_handler(14)
function_holder_alt_call is function_holder_alt.call_handler(7)
function_holder_dynamic_dict_call_label is "MethodDynamicDictFn {function_holder.dict_handler_call(false, 7)}"
function_holder_dynamic_dict_profile_label is "MethodDynamicDictProfile {length(function_holder.dict_handler_profile(true, 7))} {function_holder.dict_handler_profile(false, 7).score} {to_str(function_holder.dict_handler_profile(false, 7).name)}"
function_holder_returned_handler is function_holder.handler_value()
function_holder_return_call is function_holder_returned_handler(15)
function_holder_chosen_handler is function_holder.choose_handler(true)
function_holder_chosen_call is function_holder_chosen_handler(16)
function_holder_match_handler is function_holder.choose_handler_match(1)
function_holder_match_call is function_holder_match_handler(17)
function_holder_lookup_returned_function is function_holder.array_handler_function(false, 0)
function_holder_lookup_return_call is function_holder_lookup_returned_function(3, 4)
function_holder_lookup_return_label is "MethodArrayLookupFunctionReturn {type(function_holder_lookup_returned_function)} {to_bool(function_holder_lookup_returned_function)} {function_holder_lookup_return_call}"
binary_function_holder_call is binary_function_holder.call_handler(5, 6)
binary_function_holder_alt_call is binary_function_holder_alt.call_handler(5, 6)
binary_function_holder_chosen_handler is binary_function_holder.choose_handler(false)
binary_function_holder_chosen_call is binary_function_holder_chosen_handler(5, 6)
binary_function_holder_dict_call_label is "MethodBinaryDictFn {binary_function_holder.dict_handler_call(true, 5, 6)} {binary_function_holder.dict_handler_call(false, 5, 6)}"
binary_function_child_handler is binary_function_child.inherited_handler()
binary_function_child_call is binary_function_child_handler(5, 6)
binary_function_child_direct_call is binary_function_child.inherited_call(5, 6)
function_child_label is function_child.inherited_label()
function_child_inline_handler is function_child.inherited_inline_handler()
function_child_inline_call is function_child_inline_handler(20)
function_child_inline_label is function_child.inherited_inline_label(21)
function_child_inline_choice_label is function_child.inherited_inline_choice_label(22)
function_foreach_label is ""
for handler in function_handlers do
  function_foreach_label += type(handler)
end
function_foreach_dict_label is ""
for key, handler in function_map do
  function_foreach_dict_label += type(handler)
end
object_foreach_label is ""
for tagged_item in tagged_objects do
  object_foreach_label += tagged_item.kind_text()
end
object_foreach_dict_label is ""
for tagged_name, tagged_value in tagged_map do
  object_foreach_dict_label += tagged_value.kind_text()
end
object_foreach_return_call_label is foreach_return_tagged_ast().kind_text()
object_foreach_dict_return_call_label is foreach_return_tagged_dict_ast().kind_text()
object_param_method_label is tagged.peer_kind_label(new AstTagged()) + "!"
object_param_field_label is tagged.peer_kind_field_label(new AstTagged()) + "?"
object_param_index_label is tagged.peer_kind_index_label(new AstTagged()) + "."
object_function_param_field_label is tagged_param_field_label_ast(new AstTagged()) + "#"
object_function_param_index_label is tagged_param_index_label_ast(new AstTagged()) + ";"
object_ctor_field_method_label is tagged_box.peer_kind_label() + "~"
object_ctor_field_label is tagged_box.peer_kind_field_label() + "%"
object_ctor_index_label is tagged_box.peer_kind_index_label() + "&"
object_super_ctor_method_label is tagged_box_child.peer_kind_label() + "+"
object_super_ctor_child_method_label is tagged_box_child.child_peer_kind_label() + "="
object_super_ctor_field_label is tagged_box_child.child_peer_kind_field_label() + ":"
object_super_ctor_index_label is tagged_box_child.child_peer_kind_index_label() + ","
function_truth_score is 0
if block_double_ast then
  function_truth_score += 1
end
if function_alias and true then
  function_truth_score += 2
end
if function_map[function_key_lookup] or nil then
  function_truth_score += 4
end
while function_alias do
  function_truth_score += 8
  function_alias is nil
end
function_alias is block_double_ast
len_label is "Lengths {length(profile)} {length(names)} {length(type(profile))}"
len_mixed_ternary_label is "MixedLen {length(true ? [1, 2, 3] : {name: profile.name})} {length(false ? [1, 2, 3] : {name: profile.name})} {length(true ? \"yes\" : [1, 2])} {length(false ? \"yes\" : [1, 2])}"
str_label is "Strings {to_str(score)} {to_str(flags.active)} {to_str(flags.missing)} {to_str(profile.name)}"
literal_array_to_str_label is "Literal {to_str([score, profile.name, flags.active, flags.missing])}"
literal_dict_to_str_label is "Dict {to_str({name: profile.name})}"
runtime_collection_to_str_label is "RuntimeCollection {to_str([score, \"ok\", flags.active, flags.missing])} {to_str({score: score, active: flags.active, missing: flags.missing})}"
ternary_literal_to_str_label is "Choice {to_str(true ? [score, profile.name] : [0, \"No\"])}"
array_var_to_str_label is "ArrayVar {to_str(values)} {to_str(names)} {to_str(bools)}"
mixed_array_var_to_str_label is "MixedArrayVar {to_str(mixed_values)}"
mixed_array_index_to_str_label is "MixedIndex {to_str(mixed_values[mixed_to_str_idx])}"
mixed_array_index_runtime_label is "MixedRuntime {type(mixed_values[mixed_type_idx])} {to_bool(mixed_values[mixed_bool_idx])} {length(mixed_values[mixed_len_idx])}"
value_runtime_array_index_label is "ValueRuntimeIndex {type((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1])} {to_str((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1])} {type((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[3])} {to_str((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[3])}"
mixed_function_return_array_label is "MixedReturn {to_str(mixed_values_from_func[0])} {to_str(mixed_values_from_func[1])} {to_str(mixed_values_from_func[2])} {type(mixed_values_from_func[3])}"
direct_mixed_function_return_array_label is "DirectMixedReturn {type(make_mixed_values_ast()[0])} {to_str(make_mixed_values_ast()[1])} {to_str(make_mixed_values_ast()[2])} {type(make_mixed_values_ast()[3])}"
direct_mixed_function_return_array_to_str_label is "DirectMixedArrayString {to_str(make_mixed_values_ast())}"
direct_mixed_function_return_array_interp_label is "DirectMixedArrayInterp {make_mixed_values_ast()}"
direct_mixed_function_return_array_concat_label is "DirectMixedArrayConcat " + make_mixed_values_ast()
direct_mixed_function_return_collection_runtime_label is "DirectMixedCollectionRuntime {length(make_mixed_values_ast())} {to_bool(make_mixed_values_ast())} {length(make_mixed_profile_ast())} {to_bool(make_mixed_profile_ast())} {length(make_values_ast(false))} {to_bool(make_values_ast(false))} {length(make_profile_ast(false))} {to_bool(make_profile_ast(false))}"
param_return_collection_runtime_label is "ParamReturnCollectionRuntime {length(pass_values_ast(values))} {to_bool(pass_values_ast(values))} {length(pass_values_ast(empty_values))} {to_bool(pass_values_ast(empty_values))} {length(pass_profile_ast(meta))} {to_bool(pass_profile_ast(meta))} {length(pass_profile_ast(empty_profile))} {to_bool(pass_profile_ast(empty_profile))}"
function_return_choice_collection_to_str_label is "FunctionChoiceCollectionString {to_str(choose_values_ast(true, values))} {to_str(choose_values_ast(false, values))} {to_str(choose_profile_ast(true, meta))} {to_str(choose_profile_ast(false, meta))}"
function_return_local_choice_collection_to_str_label is "FunctionLocalChoiceCollectionString {to_str(choose_values_local_ast(true, values))} {to_str(choose_values_local_ast(false, values))} {to_str(choose_profile_local_ast(true, meta))} {to_str(choose_profile_local_ast(false, meta))}"
function_return_choice_access_label is "FunctionChoiceAccess {to_str(choose_values_ast(true, values)[0])} {type(choose_values_ast(true, values)[1])} {type(choose_profile_ast(true, meta).score)} {to_str(choose_profile_ast(true, meta)[collection_profile_key])}"
function_return_local_choice_access_label is "FunctionLocalChoiceAccess {to_str(choose_values_local_ast(true, values)[0])} {type(choose_values_local_ast(true, values)[1])} {type(choose_profile_local_ast(true, meta).score)} {to_str(choose_profile_local_ast(true, meta)[collection_profile_key])}"
function_return_alias_values is choose_values_ast(true, values)
function_return_alias_empty_values is choose_values_ast(false, values)
function_return_alias_profile is choose_profile_ast(true, meta)
function_return_alias_empty_profile is choose_profile_ast(false, meta)
function_return_alias_collection_to_str_label is "FunctionAliasCollectionString {to_str(function_return_alias_values)} {to_str(function_return_alias_empty_values)} {to_str(function_return_alias_profile)} {to_str(function_return_alias_empty_profile)}"
function_return_alias_access_label is "FunctionAliasAccess {to_str(function_return_alias_values[0])} {type(function_return_alias_profile.score)} {to_str(function_return_alias_profile[collection_profile_key])}"
function_return_local_scope_alias_label is "FunctionLocalScopeAlias {function_alias_values_text_ast(values)} {function_alias_empty_values_text_ast(values)} {function_alias_profile_text_ast(meta)} {function_alias_empty_profile_text_ast(meta)} {function_alias_profile_bonus_text_ast(meta, collection_profile_key)}"
method_return_collection_to_str_label is "MethodCollectionString {to_str(collection_tagged.full_items_value())} {to_str(collection_tagged.full_profile_value())}"
method_return_choice_collection_to_str_label is "MethodChoiceCollectionString {to_str(collection_tagged.pick_items(true))} {to_str(collection_tagged.pick_items(false))} {to_str(collection_tagged.pick_profile(true))} {to_str(collection_tagged.pick_profile(false))}"
method_return_local_choice_collection_to_str_label is "MethodLocalChoiceCollectionString {to_str(collection_tagged.pick_items_local(true))} {to_str(collection_tagged.pick_items_local(false))} {to_str(collection_tagged.pick_profile_local(true))} {to_str(collection_tagged.pick_profile_local(false))}"
method_return_choice_access_label is "MethodChoiceAccess {to_str(collection_tagged.pick_items(true)[0])} {type(collection_tagged.pick_items(true)[1])} {type(collection_tagged.pick_profile(true).score)} {to_str(collection_tagged.pick_profile(true)[collection_profile_key])}"
method_return_local_choice_access_label is "MethodLocalChoiceAccess {to_str(collection_tagged.pick_items_local(true)[0])} {type(collection_tagged.pick_items_local(true)[1])} {type(collection_tagged.pick_profile_local(true).score)} {to_str(collection_tagged.pick_profile_local(true)[collection_profile_key])}"
constructor_param_collection_label is "ConstructorParamCollection " + collection_tagged.ctor_param_summary()
super_constructor_param_collection_label is "SuperConstructorParamCollection " + super_collection_tagged.super_ctor_summary()
super_method_param_collection_label is "SuperMethodParamCollection " + super_collection_tagged.describe_via_super(values, meta)
super_method_return_collection_label is "SuperMethodReturnCollection {to_str(super_collection_tagged.values_via_super(values, meta))} {to_str(super_collection_tagged.profile_via_super(values, meta)[collection_profile_key])}"
super_update_meta is {score: 9, bonus: 3}
super_method_update_profile is super_collection_tagged.profile_via_super(values, super_update_meta)
super_method_update_profile[collection_profile_key] is 11
super_method_update_label is "SuperMethodUpdate {to_str(super_method_update_profile[collection_profile_key])}"
super_dynamic_update_meta is {score: 9, bonus: 3}
super_method_dynamic_update_key is choose_collection_profile_key_ast(true)
super_method_dynamic_update_profile is super_collection_tagged.profile_via_super(values, super_dynamic_update_meta)
super_method_dynamic_update_profile[super_method_dynamic_update_key] is 12
super_method_dynamic_update_label is "SuperMethodDynamicUpdate {to_str(super_method_dynamic_update_profile[super_method_dynamic_update_key])}"
super_call_key_string_meta is {score: 9, bonus: 3}
super_call_key_string_profile is super_collection_tagged.profile_via_super(values, super_call_key_string_meta)
super_call_key_string_profile[choose_collection_profile_key_ast(true)] is "Ada"
super_call_key_string_label is "SuperMethodCallKeyString " + super_call_key_string_profile[choose_collection_profile_key_ast(true)]
super_method_key_string_meta is {score: 9, bonus: 3}
super_method_key_string_profile is super_collection_tagged.profile_via_super(values, super_method_key_string_meta)
super_method_key_string_profile[collection_tagged.profile_key(true)] is "Eve"
super_method_key_string_label is "SuperMethodMethodKeyString " + super_method_key_string_profile[collection_tagged.profile_key(true)]
method_index_string_items is [7, 8]
method_index_string_items[collection_tagged.item_index(true)] is "Neo"
method_index_string_label is "MethodCallIndexString " + method_index_string_items[collection_tagged.item_index(true)]
method_index_mixed_items is [16, "Ada", true]
method_index_mixed_items[collection_tagged.item_index(true)] is "Neo"
method_index_value_label is "MethodCallIndexValue {type(method_index_mixed_items[collection_tagged.item_index(true)])} {to_str(method_index_mixed_items[collection_tagged.item_index(true)])} {length(method_index_mixed_items[collection_tagged.item_index(true)])}"
method_index_bool_items is [false, false]
method_index_bool_items[collection_tagged.item_index(true)] is true
method_index_bool_label is "MethodCallIndexBool {to_bool(method_index_bool_items[collection_tagged.item_index(true)])}"
method_index_tagged_items is [nil, nil]
method_index_tagged_items[collection_tagged.item_index(true)] is new AstTagged()
method_index_object_label is method_index_tagged_items[collection_tagged.item_index(true)].kind_text() + "!"
method_index_object_field_label is method_index_tagged_items[collection_tagged.item_index(true)].kind + "?"
method_index_object_index_label is method_index_tagged_items[collection_tagged.item_index(true)]["kind"] + "."
direct_mixed_function_return_dict_label is "DirectMixedDictReturn {type(make_mixed_profile_ast()[\"score\"])} {to_str(make_mixed_profile_ast()[\"name\"])} {to_str(make_mixed_profile_ast()[\"active\"])} {type(make_mixed_profile_ast()[\"missing\"])}"
direct_mixed_function_return_dict_to_str_label is "DirectMixedDictString {to_str(make_mixed_profile_ast())}"
direct_mixed_function_return_dict_interp_label is "DirectMixedDictInterp {make_mixed_profile_ast()}"
direct_mixed_function_return_dict_concat_label is "DirectMixedDictConcat " + make_mixed_profile_ast()
direct_mixed_function_return_dot_label is "DirectMixedDotReturn {type(make_mixed_profile_ast().score)} {to_str(make_mixed_profile_ast().name)} {to_str(make_mixed_profile_ast().active)} {type(make_mixed_profile_ast().missing)}"
value_runtime_dict_index_label is "ValueRuntimeKey {type((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score})[\"name\"])} {to_str((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score})[\"name\"])} {type((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score})[\"missing\"])} {to_str((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score})[\"missing\"])}"
value_runtime_dot_label is "ValueRuntimeDot {type((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).name)} {to_str((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).name)} {type((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).missing)} {to_str((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).missing)}"
value_runtime_access_interp_label is "ValueRuntimeAccessInterp {(score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1]} {(score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).name} {(score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).missing}"
value_runtime_access_call_label is "ValueRuntimeAccessCall {length((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1])} {to_bool((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1])} {length((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).name)} {to_bool((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).name)} {to_bool((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).missing)}"
value_runtime_access_eq_label is "ValueRuntimeAccessEq {((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1]) == \"one\"} {((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[3]) == nil} {((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).name) == \"Ada\"} {((score > 100 ? [\"bad\"] : {name: \"Ada\", score: score}).missing) != nil}"
value_runtime_access_numeric_label is "ValueRuntimeNumeric {((score > 100 ? [\"bad\"] : {score: score}).score) + 4} {((score > 100 ? [\"bad\"] : {score: score}).score) >= 16} {((score < 100 ? [7, 16, 21] : {name: \"bad\"})[1]) * 2} {((score > 100 ? [\"bad\"] : {score: score}).score) % 5}"
value_runtime_access_numeric_ternary_label is "ValueRuntimeNumericTernary {((true ? ((score > 100 ? [\"bad\"] : {score: score}).score) : 0) + 1)} {((false ? 0 : ((score < 100 ? [7, 16, 21] : {name: \"bad\"})[1])) + 2)}"
value_runtime_access_numeric_coalesce_label is "ValueRuntimeNumericCoalesce {(((score > 100 ? [\"bad\"] : {score: score}).missing) ?? 5) + 1} {(((score > 100 ? [\"bad\"] : {score: score}).score) ?? 0) + 2} {(((score < 100 ? [7, 16, 21] : {name: \"bad\"})[3]) ?? 9) + 1} {(((score < 100 ? [7, 16, 21] : {name: \"bad\"})[1]) ?? 0) + 3}"
value_runtime_coalesce_receiver_label is "ValueRuntimeCoalesceReceiver {type(({name: \"Ada\", score: score} ?? [\"bad\"]).name)} {to_str(({name: \"Ada\", score: score} ?? [\"bad\"]).name)} {type(([7, 16, 21] ?? {name: \"bad\"})[1])} {to_str(([7, 16, 21] ?? {name: \"bad\"})[1])} {type((nil ?? {name: \"Fallback\"}).name)} {to_str((nil ?? {name: \"Fallback\"}).name)}"
value_runtime_nested_access_label is "ValueRuntimeNested {((score > 100 ? [\"bad\"] : {player: {name: \"Ada\", stats: [7, 16, 21]}}).player).name} {((score > 100 ? [\"bad\"] : {player: {name: \"Ada\", stats: [7, 16, 21]}}).player).stats[1]} {type(((score > 100 ? [\"bad\"] : {player: {name: \"Ada\", stats: [7, 16, 21]}}).missing).name)} {to_str(((score > 100 ? [\"bad\"] : {player: {name: \"Ada\", stats: [7, 16, 21]}}).missing).name)}"
value_runtime_same_dict_shape_label is "ValueRuntimeSameDictShape {type((score < 100 ? {score: score} : {name: \"bad\"}).score)} {to_str((score < 100 ? {score: score} : {name: \"bad\"}).score)} {type((score < 100 ? {score: score} : {name: \"bad\"}).name)} {to_str((score < 100 ? {score: score} : {name: \"bad\"}).name)} {type((score < 100 ? {score: score} : {score: \"bad\"}).score)} {to_str((score < 100 ? {score: score} : {score: \"bad\"}).score)}"
value_runtime_same_array_shape_label is "ValueRuntimeSameArrayShape {type((score < 100 ? [score, \"ok\"] : [\"bad\", 0])[0])} {to_str((score < 100 ? [score, \"ok\"] : [\"bad\", 0])[0])} {type((score < 100 ? [score] : [\"bad\", true])[1])} {to_str((score < 100 ? [score] : [\"bad\", true])[1])} {type((score > 100 ? [score] : [\"bad\", true])[1])} {to_str((score > 100 ? [score] : [\"bad\", true])[1])}"
value_runtime_coalesce_shape_label is "ValueRuntimeCoalesceShape {type(({score: score} ?? {name: \"bad\"}).score)} {to_str(({score: score} ?? {name: \"bad\"}).score)} {type(({score: score} ?? {name: \"bad\"}).name)} {to_str(({score: score} ?? {name: \"bad\"}).name)} {type(([score] ?? [\"bad\", true])[1])} {to_str(([score] ?? [\"bad\", true])[1])} {type(([score, \"ok\"] ?? [\"bad\", true])[0])} {to_str(([score, \"ok\"] ?? [\"bad\", true])[0])}"
value_runtime_string_method_label is "ValueRuntimeStringMethod {((score < 100 ? [\"  ok  \"] : {name: \"bad\"})[0]).trim()} {((score > 100 ? [\"bad\"] : {name: \"  ada  \"}).name).trim().upper()} {((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0]).contains(\"wasm\")} {((score > 100 ? [\"bad\"] : {name: \"sura wasm\"}).name).indexOf(\"wasm\")}"
value_runtime_string_chain_label is "ValueRuntimeStringChain {((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0]).replace(\"wasm\", \"WASM\").upper()} {((score > 100 ? [\"bad\"] : {name: \"SURA WASM\"}).name).substring(5).lower()} {((score > 100 ? [\"bad\"] : {name: \"sura wasm\"}).name).slice(5).contains(\"wasm\")} {((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0]).replace(\"sura\", \"SURA\").indexOf(\"WASM\")}"
value_runtime_string_call_label is "ValueRuntimeStringCall {upper((score < 100 ? [\"ok\"] : {name: \"bad\"})[0])} {string.lower((score > 100 ? [\"bad\"] : {name: \"SURA\"}).name)} {trim((score > 100 ? [\"bad\"] : {name: \"  ada  \"}).name)} {string.replace((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], \"sura\", \"SURA\")} {contains((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], \"wasm\")} {string.indexOf((score > 100 ? [\"bad\"] : {name: \"sura wasm\"}).name, \"wasm\")} {substring((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], 5)}"
value_runtime_len_method_label is "ValueRuntimeLenMethod {((score < 100 ? [\"zero\", \"one\"] : {name: \"bad\"})[1]).len()} {((score > 100 ? [\"bad\"] : {name: \"Ada\"}).name).length()} {((score > 100 ? [\"bad\"] : {name: \"Ada\"}).missing).len()} {((score > 100 ? [\"bad\"] : {items: [7, 16, 21]}).items).length()}"
value_runtime_alias is (score < 100 ? ["zero", "one"] : {name: "bad"})[1]
value_runtime_missing_alias is (score < 100 ? ["zero", "one"] : {name: "bad"})[3]
value_runtime_dot_alias is (score > 100 ? ["bad"] : {name: "Ada", score: score}).name
value_runtime_num_alias is (score > 100 ? ["bad"] : {score: score}).score
value_runtime_string_search_alias_variant_label is "ValueRuntimeStringSearchAliasVariant {string_indexOf((score > 100 ? [\"bad\"] : {name: \"sura wasm\"}).name, \"wasm\")} {string_startsWith((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], \"sura\")} {string_endsWith((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], \"wasm\")} {string.indexOf((score > 100 ? [\"bad\"] : {name: \"sura wasm\"}).name, \"wasm\")} {string.startsWith((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], \"sura\")} {string.endsWith((score < 100 ? [\"sura wasm\"] : {name: \"bad\"})[0], \"wasm\")}"
value_runtime_alias_label is "ValueRuntimeAlias {type(value_runtime_alias)} {to_str(value_runtime_alias)} {to_bool(value_runtime_alias)} {length(value_runtime_alias)} {value_runtime_alias == \"one\"} {value_runtime_alias + \"!\"} {type(value_runtime_missing_alias)} {to_str(value_runtime_missing_alias)} {type(value_runtime_dot_alias)} {to_str(value_runtime_dot_alias)} {value_runtime_num_alias + 4}"
value_runtime_alias_method_label is "ValueRuntimeAliasMethod {value_runtime_alias.upper()} {value_runtime_alias.startsWith(\"on\")} {value_runtime_alias.replace(\"one\", \"ONE\").lower()} {string.upper(value_runtime_dot_alias)} {value_runtime_alias.length()} {string.contains(value_runtime_alias, \"ne\")}"
value_runtime_collection_alias is score < 100 ? ["zero", "one"] : {name: "bad"}
value_runtime_number_collection_alias is score < 100 ? [7, 16, 21] : {name: "bad"}
value_runtime_profile_alias is score > 100 ? ["bad"] : {name: "Ada", score: score}
value_runtime_collection_alias_label is "ValueRuntimeCollectionAlias {type(value_runtime_collection_alias)} {length(value_runtime_collection_alias)} {to_str(value_runtime_collection_alias[1])} {type(value_runtime_collection_alias[3])} {to_str(value_runtime_profile_alias.name)} {type(value_runtime_profile_alias.missing)} {value_runtime_profile_alias.score + 4} {value_runtime_collection_alias[1].upper()} {string.contains(value_runtime_profile_alias.name, \"da\")} {value_runtime_collection_alias.contains(\"one\")} {value_runtime_collection_alias.index_of(\"one\")}"
value_runtime_collection_len_method_label is "ValueRuntimeCollectionLenMethod {value_runtime_collection_alias.len()} {value_runtime_collection_alias.length()} {value_runtime_number_collection_alias.len()} {value_runtime_number_collection_alias.length()}"
value_runtime_number_collection_alias_label is "ValueRuntimeNumberCollectionAlias {value_runtime_number_collection_alias.sum()} {value_runtime_number_collection_alias.avg()} {value_runtime_number_collection_alias.min()} {value_runtime_number_collection_alias.max()}"
value_runtime_mutable_collection is score < 100 ? ["zero", "one"] : {name: "bad"}
value_runtime_mutable_collection[1] is "two"
value_runtime_mutable_profile is score > 100 ? ["bad"] : {name: "Ada", score: score}
value_runtime_mutable_profile["score"] is 20
value_runtime_mutable_profile.name is "Grace"
value_runtime_mutable_label is "ValueRuntimeMutable {value_runtime_mutable_collection[1]} {value_runtime_mutable_profile.name} {value_runtime_mutable_profile.score}"
value_runtime_array_module_len_alias is array.len(value_runtime_number_collection_alias)
value_runtime_array_module_sum_alias is array.sum(value_runtime_number_collection_alias)
value_runtime_array_module_avg_alias is array.avg(value_runtime_number_collection_alias)
value_runtime_array_module_min_alias is array.min(value_runtime_number_collection_alias)
value_runtime_array_module_max_alias is array.max(value_runtime_number_collection_alias)
value_runtime_array_module_contains_alias is array.contains(value_runtime_collection_alias, "one")
value_runtime_array_module_index_alias is array.index_of(value_runtime_collection_alias, "one")
value_runtime_array_module_alias_label is "ValueRuntimeArrayModuleAlias {value_runtime_array_module_len_alias} {value_runtime_array_module_sum_alias} {value_runtime_array_module_avg_alias} {value_runtime_array_module_min_alias} {value_runtime_array_module_max_alias} {value_runtime_array_module_contains_alias} {value_runtime_array_module_index_alias}"
value_runtime_array_direct_len_alias is array_len(value_runtime_number_collection_alias)
value_runtime_array_direct_sum_alias is array_sum(value_runtime_number_collection_alias)
value_runtime_array_direct_avg_alias is array_avg(value_runtime_number_collection_alias)
value_runtime_array_direct_min_alias is array_min(value_runtime_number_collection_alias)
value_runtime_array_direct_max_alias is array_max(value_runtime_number_collection_alias)
value_runtime_array_direct_contains_alias is array_contains(value_runtime_collection_alias, "one")
value_runtime_array_direct_index_alias is array_index_of(value_runtime_collection_alias, "one")
value_runtime_array_direct_alias_label is "ValueRuntimeArrayDirectAlias {value_runtime_array_direct_len_alias} {value_runtime_array_direct_sum_alias} {value_runtime_array_direct_avg_alias} {value_runtime_array_direct_min_alias} {value_runtime_array_direct_max_alias} {value_runtime_array_direct_contains_alias} {value_runtime_array_direct_index_alias}"
value_runtime_array_alias_variant_length is array_length(value_runtime_number_collection_alias)
value_runtime_array_alias_variant_average is array_average(value_runtime_number_collection_alias)
value_runtime_array_alias_variant_index is array_index(value_runtime_collection_alias, "one")
value_runtime_array_alias_variant_index_of_camel is array_indexOf(value_runtime_collection_alias, "one")
value_runtime_array_alias_variant_includes is array_includes(value_runtime_collection_alias, "one")
value_runtime_array_module_variant_length is array.length(value_runtime_number_collection_alias)
value_runtime_array_module_variant_average is array.average(value_runtime_number_collection_alias)
value_runtime_array_module_variant_index_of_camel is array.indexOf(value_runtime_collection_alias, "one")
value_runtime_array_module_variant_includes is array.includes(value_runtime_collection_alias, "one")
value_runtime_array_alias_variant_label is "ValueRuntimeArrayAliasVariant {value_runtime_array_alias_variant_length} {value_runtime_array_alias_variant_average} {value_runtime_array_alias_variant_index} {value_runtime_array_alias_variant_index_of_camel} {value_runtime_array_alias_variant_includes} {value_runtime_array_module_variant_length} {value_runtime_array_module_variant_average} {value_runtime_array_module_variant_index_of_camel} {value_runtime_array_module_variant_includes}"
value_runtime_return_collection_alias is make_value_collection_alias_ast(true)
value_runtime_return_profile_alias is make_value_profile_alias_ast(true, score)
value_runtime_return_alias_label is "ValueRuntimeReturnAlias {type(value_runtime_return_collection_alias)} {length(value_runtime_return_collection_alias)} {to_str(value_runtime_return_collection_alias[1])} {to_str(value_runtime_return_profile_alias.name)} {type(value_runtime_return_profile_alias.missing)} {value_runtime_return_profile_alias.score + 4}"
value_runtime_return_collection_alias[1] is "TWO"
value_runtime_return_profile_alias["score"] is 24
value_runtime_return_profile_alias.name is "Lin"
value_runtime_return_mutable_label is "ValueRuntimeReturnMutable {value_runtime_return_collection_alias[1]} {value_runtime_return_profile_alias.name} {value_runtime_return_profile_alias.score}"
value_runtime_method_collection_alias is collection_tagged.pick_mixed_value_items(true)
value_runtime_method_profile_alias is collection_tagged.pick_mixed_value_profile(true)
value_runtime_method_collection_alias[1] is "DOS"
value_runtime_method_profile_alias["score"] is 28
value_runtime_method_profile_alias.name is "Kim"
value_runtime_method_mutable_label is "ValueRuntimeMethodMutable {value_runtime_method_collection_alias[1]} {value_runtime_method_profile_alias.name} {value_runtime_method_profile_alias.score}"
value_runtime_super_collection_alias is super_collection_tagged.mixed_items_via_super(true)
value_runtime_super_profile_alias is super_collection_tagged.mixed_profile_via_super(true)
value_runtime_super_collection_alias[1] is "TRES"
value_runtime_super_profile_alias["score"] is 32
value_runtime_super_profile_alias.name is "Seo"
value_runtime_super_mutable_label is "ValueRuntimeSuperMutable {value_runtime_super_collection_alias[1]} {value_runtime_super_profile_alias.name} {value_runtime_super_profile_alias.score}"
mixed_dict_index_runtime_label is "DictRuntime {type(mixed_profile[mixed_profile_active_lookup])} {to_bool(mixed_profile[mixed_profile_active_lookup])} {length(mixed_profile[mixed_profile_name_lookup])} {to_str(mixed_profile[mixed_profile_score_lookup])} {to_str(mixed_profile[mixed_profile_missing_lookup])}"
mixed_exact_not_label is "NotExact {not mixed_values[mixed_bool_idx]} {not mixed_profile[mixed_profile_active_lookup]} {not mixed_profile[mixed_profile_missing_lookup]}"
mixed_exact_logic_label is "LogicExact {mixed_values[mixed_bool_idx] and true} {mixed_profile[mixed_profile_active_lookup] and true} {mixed_profile[mixed_profile_missing_lookup] or true} {mixed_profile[mixed_profile_missing_lookup] and true}"
mixed_exact_eq_label is "EqExact {mixed_values[mixed_bool_idx] == true} {mixed_profile[mixed_profile_active_lookup] == true} {mixed_profile[mixed_profile_missing_lookup] == nil} {mixed_profile[mixed_profile_score_lookup] != 17}"
mixed_exact_compare_label is "CompareExact {mixed_values[mixed_score_idx] >= 16} {mixed_values[mixed_score_idx] < 20} {mixed_profile[mixed_profile_score_lookup] > 10} {mixed_profile[mixed_profile_score_lookup] <= 16}"
mixed_exact_arith_label is "ArithExact {mixed_values[mixed_score_idx] + 4} {mixed_profile[mixed_profile_score_lookup] - 6} {mixed_values[mixed_score_idx] * 2} {mixed_profile[mixed_profile_score_lookup] / 4} {mixed_profile[mixed_profile_score_lookup] % 5}"
mixed_exact_bitwise_label is "BitExact {mixed_values[mixed_score_idx] & 7} {mixed_profile[mixed_profile_score_lookup] | 3} {mixed_values[mixed_score_idx] ^ 3} {mixed_profile[mixed_profile_score_lookup] << 1} {mixed_values[mixed_score_idx] >> 1} {~mixed_values[mixed_score_idx]}"
dict_var_to_str_label is "DictVar {to_str(mixed_profile)}"
nested_collection_to_str_label is "NestedCollection {to_str([names, mixed_profile])} {to_str({names: names, profile: mixed_profile})}"
to_str_mixed_ternary_label is "MixedStr {to_str(true ? \"yes\" : nil)} {to_str(false ? \"yes\" : nil)} {to_str(true ? [score, profile.name] : {name: profile.name})} {to_str(false ? [score, profile.name] : {name: profile.name})}"
type_mixed_ternary_label is "MixedType {type(true ? [1, 2] : {name: profile.name})} {type(false ? [1, 2] : {name: profile.name})} {type(true ? \"yes\" : nil)} {type(false ? \"yes\" : nil)}"
bool_label is "Bools {to_bool(profile)} {to_bool(empty_profile)} {to_bool(flags.missing)}"
compare_label is "Compare {score == 16} Low {score < 20} High {score > 20}"
tag_label is "Kind {tagged.kind} Active {tagged.active} Missing {tagged.missing}"
object_index_label is tagged_objects[0].kind_text()
object_dot_label is tagged_map.primary.kind_text()
object_key_label is tagged_map["secondary"].kind_text()
object_dynamic_index_label is tagged_objects[dynamic_idx].kind_text()
object_dynamic_key_label is tagged_map[tagged_key_lookup].kind_text()
dynamic_tagged_map is {}
dynamic_tagged_map[dynamic_tagged_update_key] is new AstTagged()
object_dynamic_assigned_key_label is dynamic_tagged_map[dynamic_tagged_update_key].kind_text()
object_dynamic_assigned_field_label is dynamic_tagged_map[dynamic_tagged_update_key].kind + "^"
object_dynamic_assigned_index_label is dynamic_tagged_map[dynamic_tagged_update_key]["kind"] + "$"
mixed_key_lookup is "label"
mixed_dynamic_dict is {count: 3, ready: true}
mixed_dynamic_dict[mixed_key_lookup] is "Curie"
dynamic_dict_assigned_string_label is "Dict " + mixed_dynamic_dict[mixed_key_lookup]
mixed_num_key_lookup is "points"
mixed_num_dict is {count: 11, label: "skip"}
mixed_num_dict[mixed_num_key_lookup] is score
dynamic_dict_assigned_num_label is "NumDict " + mixed_num_dict[mixed_num_key_lookup]
dynamic_dict_assigned_num_calc is mixed_num_dict[mixed_num_key_lookup] + 4
mixed_bool_key_lookup is "flag"
mixed_bool_dict is {count: 7, label: "skip"}
mixed_bool_dict[mixed_bool_key_lookup] is false
dynamic_dict_assigned_bool_label is "Flag " + mixed_bool_dict[mixed_bool_key_lookup]
mixed_nil_key_lookup is "missing"
mixed_nil_dict is {count: 9, label: "skip"}
mixed_nil_dict[mixed_nil_key_lookup] is nil
dynamic_dict_assigned_nil_label is "Missing " + mixed_nil_dict[mixed_nil_key_lookup]
mixed_object_key_lookup is "bot"
mixed_object_dict is {count: 2, label: "skip"}
mixed_object_dict[mixed_object_key_lookup] is new AstTagged()
object_dynamic_exact_dict_label is mixed_object_dict[mixed_object_key_lookup].kind_text()
mixed_array_key_lookup is "items"
mixed_array_dict is {count: 5, label: "skip"}
mixed_array_dict[mixed_array_key_lookup] is [4, 5, 6]
dynamic_dict_assigned_array_label is "Array {length(mixed_array_dict[mixed_array_key_lookup])} {to_bool(mixed_array_dict[mixed_array_key_lookup])}"
mixed_nested_key_lookup is "profile"
mixed_nested_dict is {count: 1, label: "skip"}
mixed_nested_dict[mixed_nested_key_lookup] is {ready: 1, bonus: 2}
dynamic_dict_assigned_dict_label is "Dict {length(mixed_nested_dict[mixed_nested_key_lookup])} {to_bool(mixed_nested_dict[mixed_nested_key_lookup])}"
mixed_function_key_lookup is "handler"
mixed_function_dict is {count: 4, label: "skip"}
mixed_function_dict[mixed_function_key_lookup] is block_double_ast
dynamic_dict_assigned_function_label is "Func {type(mixed_function_dict[mixed_function_key_lookup])} {to_bool(mixed_function_dict[mixed_function_key_lookup])}"
dynamic_dict_assigned_function_same is mixed_function_dict[mixed_function_key_lookup] == block_double_ast
dynamic_tagged_items is [nil, nil]
dynamic_tagged_items[dynamic_idx] is new AstTagged()
object_dynamic_assigned_array_label is dynamic_tagged_items[dynamic_idx].kind_text()
object_dynamic_assigned_array_field_label is dynamic_tagged_items[dynamic_idx].kind + "@"
object_dynamic_assigned_array_index_label is dynamic_tagged_items[dynamic_idx]["kind"] + "/"
dynamic_string_items is [nil, nil]
dynamic_string_items[dynamic_idx] is "Curie"
dynamic_assigned_string_label is "Assigned " + dynamic_string_items[dynamic_idx]
dynamic_bool_items is [nil, nil]
dynamic_bool_items[dynamic_idx] is true
dynamic_assigned_bool_label is "Bool " + dynamic_bool_items[dynamic_idx]
dynamic_nil_items is [true, true]
dynamic_nil_items[dynamic_idx] is nil
dynamic_assigned_nil_label is "Nil " + dynamic_nil_items[dynamic_idx]
dynamic_number_items is [0, 0]
dynamic_number_items[dynamic_idx] is score
dynamic_assigned_num_label is "Number " + dynamic_number_items[dynamic_idx]
dynamic_assigned_num_calc is dynamic_number_items[dynamic_idx] + 4
param_tag_label is "Kind {param_tagged.kind} Active {param_tagged.active} Missing {param_tagged.missing}"
direct_interp_dot_label is "Direct {make_tagged_local_ast().kind}"
direct_interp_method_label is "Method {make_tagged_local_ast().kind_text()}"
direct_interp_ternary_label is "Active {(true ? new AstTagged() : new AstTagged()).active}"
direct_interp_index_label is "Index {make_tagged_local_ast()[\"kind\"]}"
direct_interp_ternary_index_label is "Active {(true ? new AstTagged() : new AstTagged())[\"active\"]} Missing {(false ? new AstTagged() : new AstTagged())[\"missing\"]}"
pi_label is "Pi {math.pi}"
literal_braces is "json {} done"
score_concat_label is "score " + score + "!"
mutable_label is "Go"
mutable_label += "!"
mutable_label += score
score_suffix is score
score_suffix += "!"
compound_op_score is 30
compound_op_score -= 4
compound_op_score *= 3
compound_op_score /= 2
compound_op_score %= 10
compound_ops_label is "CompoundOps {compound_op_score}"
name_join is ""
for person in names do
  name_join += person
end
indexed_name_join is ""
name_index_sum is 0
for name_idx, indexed_person in names do
  name_index_sum += name_idx
  indexed_name_join += indexed_person
end
inline_name_join is ""
for part in ["S", "R"] do
  inline_name_join += part
end
profile_key_join is ""
profile_value_join is ""
for profile_key, profile_value in profile do
  profile_key_join += profile_key
  profile_value_join += profile_value
end
meta_key_join is ""
meta_value_sum is 0
for meta_key, meta_value in meta do
  meta_key_join += meta_key
  meta_value_sum += meta_value
end
dict_single_count is 0
for ignored_profile in profile do
  dict_single_count += 1
end
assert_eq(block_double_ast(6), 12)
assert_eq(point.x, 6)
assert_eq(point.y, 7)
assert_eq(point.sum(), 13)
assert_eq(point.shifted(2), 15)
assert_eq(point.shifted_conflict(3), 9)
assert_eq(point.shifted_conflict("bad"), "6bad")
assert_eq(dynamic_method_score_ast(true), 14)
assert_eq(dynamic_method_score_ast(false), 21)
assert_eq(dynamic_inherited_score_ast(true), 11)
assert_eq(dynamic_inherited_score_ast(false), 14)
assert_eq(recursive_counter.countdown(2), 4)
assert_eq(child.root, 2)
assert_eq(child.leaf, 5)
assert_eq(child.root_value(), 2)
assert_eq(child.root_plus_one(), 3)
assert_eq(child.root_plus_delta(4), 6)
assert_eq(child.root_label_child(), "Root 2!")
assert(tagged.kind == "bot")
assert(tagged.active == true)
assert(tagged.missing == nil)
assert_eq(tagged.kind, "bot")
assert_eq(tagged.active, true)
assert_eq(tagged.missing, nil)
assert_eq(tagged.kind + " unit", "bot unit")
assert_eq("Tagged: " + tagged.kind + " " + tagged.active + " " + tagged.missing, "Tagged: bot true nil")
assert_eq(tag_label, "Kind bot Active true Missing nil")
assert(tagged.kind_text() == "bot")
assert(tagged.is_active() == true)
assert(tagged.none_value() == nil)
assert_eq(tagged.kind_text(), "bot")
assert_eq(tagged.is_active(), true)
assert_eq(tagged.none_value(), nil)
assert_eq(tagged.kind_text() + " method", "bot method")
assert_eq("Method: " + tagged.kind_text() + " " + tagged.is_active() + " " + tagged.none_value(), "Method: bot true nil")
assert_eq(object_index_label, "bot")
assert_eq(object_dot_label, "bot")
assert_eq(object_key_label, "bot")
assert_eq(object_dynamic_index_label, "bot")
assert_eq(object_dynamic_key_label, "bot")
assert_eq(object_dynamic_assigned_key_label, "bot")
assert_eq(object_dynamic_assigned_field_label, "bot^")
assert_eq(object_dynamic_assigned_index_label, "bot$")
assert_eq(dynamic_dict_assigned_string_label, "Dict Curie")
assert_eq(dynamic_dict_assigned_num_label, "NumDict 16")
assert_eq(dynamic_dict_assigned_num_calc, 20)
assert_eq(dynamic_dict_assigned_bool_label, "Flag false")
assert_eq(dynamic_dict_assigned_nil_label, "Missing nil")
assert_eq(object_dynamic_exact_dict_label, "bot")
assert_eq(dynamic_dict_assigned_array_label, "Array 3 true")
assert_eq(dynamic_dict_assigned_dict_label, "Dict 2 true")
assert_eq(dynamic_dict_assigned_function_label, "Func function true")
assert_eq(dynamic_dict_assigned_function_same, true)
assert_eq(object_dynamic_assigned_array_label, "bot")
assert_eq(object_dynamic_assigned_array_field_label, "bot@")
assert_eq(object_dynamic_assigned_array_index_label, "bot/")
assert_eq(dynamic_assigned_string_label, "Assigned Curie")
assert_eq(dynamic_assigned_bool_label, "Bool true")
assert_eq(dynamic_assigned_nil_label, "Nil nil")
assert_eq(dynamic_assigned_num_label, "Number 16")
assert_eq(dynamic_assigned_num_calc, 20)
assert(param_tagged.kind == "agent")
assert(param_tagged.active == true)
assert(param_tagged.missing == nil)
assert_eq(param_tagged.kind, "agent")
assert_eq(param_tagged.active, true)
assert_eq(param_tagged.missing, nil)
assert_eq(param_tagged.kind + " core", "agent core")
assert_eq("Param: " + param_tagged.kind + " " + param_tagged.active + " " + param_tagged.missing, "Param: agent true nil")
assert_eq(param_tag_label, "Kind agent Active true Missing nil")
assert_eq(direct_interp_dot_label, "Direct bot")
assert_eq(direct_interp_method_label, "Method bot")
assert_eq(direct_interp_ternary_label, "Active true")
assert_eq(direct_interp_index_label, "Index bot")
assert_eq(direct_interp_ternary_index_label, "Active true Missing nil")
assert_eq(pi_label, "Pi 3.141592653589793")
assert(param_tagged.kind_text() == "agent")
assert(param_tagged.is_active() == true)
assert(param_tagged.none_value() == nil)
assert_eq(param_tagged.kind_text(), "agent")
assert_eq(param_tagged.is_active(), true)
assert_eq(param_tagged.none_value(), nil)
assert_eq(param_tagged.kind_text() + " method", "agent method")
assert_eq("Param Method: " + param_tagged.kind_text() + " " + param_tagged.is_active() + " " + param_tagged.none_value(), "Param Method: agent true nil")
assert_eq(param_tagged.kind_badge(), "agent badge")
assert_eq(param_tagged.state_line(), "State true nil")
assert_eq(label_pair("agent ", "ready"), "agent ready")
assert_eq(nested_label_pair("nested ", "ready"), "nested ready")
assert_eq(bool_state_line("Flags ", true, nil), "Flags true nil")
assert_eq(bool_gate_ast(true, nil), true)
assert_eq(bool_gate_ast(false, nil), false)
assert_eq("Gate: " + bool_gate_ast(true, nil), "Gate: true")
assert_eq(format_pair_ast("fmt ", "ok"), "fmt ok")
assert_eq(local_tagged_kind_len_ast(true), 3)
assert_eq(local_tagged_field_len_ast(false), 12)
assert(local_tagged_gate_ast(true))
assert_eq(local_tagged_from_call_len_ast(true), 3)
assert_eq(local_tagged_from_if_len_ast(false), 3)
assert_eq(local_tagged_from_match_len_ast(2), 3)
assert_eq(local_tagged_from_method_len_ast(false), 3)
assert_eq(tagged_param_kind_len_ast(tagged), 3)
assert_eq(tagged_param_peer_len_ast(tagged), 3)
assert_eq(tagged.peer_kind_len(make_tagged_local_ast()), 3)
assert_eq(direct_tagged_call_len_ast(), 3)
assert_eq(direct_tagged_ternary_len_ast(false), 3)
assert_eq(direct_tagged_receiver_arg_len_ast(), 3)
assert_eq(direct_tagged_index_label_ast(), "bot!")
assert_eq(direct_tagged_ternary_index_label_ast(true), "State true nil")
assert_eq(conflict_probe(4), 5)
assert_eq(conflict_probe("agent"), "agent1")
assert_eq(param_tagged.kind_suffix(" ready"), "agent ready")
assert_eq(param_tagged.state_prefix("Flags "), "Flags true nil")
assert_eq(tagged.active_gate(), true)
assert_eq(param_tagged.active_gate(), true)
assert_eq("Gates: " + tagged.active_gate() + " " + param_tagged.active_gate(), "Gates: true true")
assert_eq(field_conflict_num.bump(), 5)
assert_eq(field_conflict_text.bump(), "bad1")
assert("ready" == "ready")
assert("ready" != "done")
assert(nil == nil)
assert(true == true)
assert_eq("ready", "ready")
assert_ne("ready", "done")
assert_eq(nil, nil)
assert_eq(true, true)
truth_score is 0
if "ready" then
  truth_score += 1
end
if "" then
  truth_score += 10
else
  truth_score += 2
end
if nil then
  truth_score += 20
else
  truth_score += 4
end
truth_pick is "go" ? 8 : 9
nil_pick is nil ? 10 : 16
ternary_label is true ? "yes" : "no"
ternary_bool is true ? true : false
ternary_nil is false ? nil : nil
assert_eq(truth_score, 7)
assert_eq(truth_pick + nil_pick, 24)
assert_eq(ternary_label, "yes")
assert_eq(ternary_label + "!", "yes!")
assert_eq(ternary_bool, true)
assert_eq(ternary_nil, nil)
assert_eq("Ternary: " + ternary_bool + " " + ternary_nil, "Ternary: true nil")
assert_eq(pick_label_ast(true), "on")
assert_eq("Pick: " + pick_label_ast(false), "Pick: off")
assert_eq(pick_bool_ast(true), true)
assert("ready" and true)
assert(not ("" and true))
assert("ready" or nil)
assert(not ("" or nil))
assert_eq("ready" and true, true)
assert_eq("" and true, false)
assert_eq("ready" or nil, true)
assert_eq("" or nil, false)
short_circuit_and_ok is false and (1 / 0 == 0)
short_circuit_or_ok is true or (1 / 0 == 0)
assert_eq(short_circuit_and_ok, false)
assert_eq(short_circuit_or_ok, true)
assert_eq([1] and true, true)
assert_eq([] or false, false)
assert_eq({ready: 1} and true, true)
assert_eq({} or false, false)
assert([1])
assert(not [])
assert({ready: 1})
assert(not {})
assert_eq((true ? "direct" : "fallback") + "!", "direct!")
assert(true ? [1] : [])
assert(not (false ? [1] : []))
assert(true ? {ready: 1} : {})
assert(not (false ? {ready: 1} : {}))
assert(true ? [1] : nil)
assert(not (false ? [1] : nil))
assert(true ? {ready: 1} : nil)
assert(not (false ? {ready: 1} : nil))
assert_eq(true ? "mixed" : nil, "mixed")
assert_eq(false ? "mixed" : nil, nil)
assert_ne(false ? nil : 12, nil)
assert_eq((true ? "mixed" : nil) + "!", "mixed!")
assert_eq((false ? "mixed" : nil) + "!", "nil!")
assert_eq((false ? "mixed" : true) + "!", "true!")
assert_eq((false ? "mixed" : 12) + "!", "12!")
assert(values)
assert(not empty_values)
assert(meta)
assert(not empty_profile)
assert(make_values_ast(true))
assert(not make_values_ast(false))
assert(make_profile_ast(true))
assert(not make_profile_ast(false))
assert(pass_values_ast(values))
assert(not pass_values_ast(empty_values))
assert(pass_profile_ast(meta))
assert(not pass_profile_ast(empty_profile))
assert(choose_values_ast(true, values))
assert(not choose_values_ast(false, values))
assert(choose_profile_ast(true, meta))
assert(not choose_profile_ast(false, meta))
assert(choose_values_local_ast(true, values))
assert(not choose_values_local_ast(false, values))
assert(choose_profile_local_ast(true, meta))
assert(not choose_profile_local_ast(false, meta))
assert_eq(choose_values_local_gate_ast(true, values), true)
assert_eq(choose_values_local_gate_ast(false, values), false)
assert_eq(choose_profile_local_gate_ast(true, meta), true)
assert_eq(choose_profile_local_gate_ast(false, meta), false)
assert(collection_tagged.full_items)
assert(not collection_tagged.empty_items)
assert(collection_tagged.full_profile)
assert(not collection_tagged.empty_profile)
assert(collection_tagged.full_items_value())
assert(not collection_tagged.empty_items_value())
assert(collection_tagged.full_profile_value())
assert(not collection_tagged.empty_profile_value())
assert(collection_tagged.pick_items(true))
assert(not collection_tagged.pick_items(false))
assert(collection_tagged.pick_profile(true))
assert(not collection_tagged.pick_profile(false))
assert(collection_tagged.pick_items_local(true))
assert(not collection_tagged.pick_items_local(false))
assert(collection_tagged.pick_profile_local(true))
assert(not collection_tagged.pick_profile_local(false))
assert_eq(collection_tagged.pick_items_local_gate(true), true)
assert_eq(collection_tagged.pick_items_local_gate(false), false)
assert_eq(collection_tagged.pick_profile_local_gate(true), true)
assert_eq(collection_tagged.pick_profile_local_gate(false), false)
assert(nested.items)
assert(not nested.empty_items)
assert(nested.profile)
assert(not nested.empty_profile)
assert(nested["items"])
assert(not nested["empty_items"])
assert(nested_arrays[0])
assert(not nested_arrays[1])
assert(nested_profiles[0])
assert(not nested_profiles[1])
nested.items is []
nested.empty_items is [2]
nested["profile"] is {}
nested["empty_profile"] is {ready: 2}
nested_arrays[0] is []
nested_arrays[1] is [3]
nested_profiles[0] is {}
nested_profiles[1] is {ready: 3}
assert(not nested.items)
assert(nested.empty_items)
assert(not nested["profile"])
assert(nested["empty_profile"])
assert(not nested_arrays[0])
assert(nested_arrays[1])
assert(not nested_profiles[0])
assert(nested_profiles[1])
assert(not "")
assert(not nil)
assert_eq(not "", true)
assert_eq(not "ready", false)
assert_eq(not [], true)
assert_eq(not [1], false)
assert_eq(not {}, true)
assert_eq(not {ready: 1}, false)
assert_eq(meta.score, 9)
assert_eq(meta["bonus"], 3)
assert(profile.name == "Ada")
assert(profile["city"] == "Seoul")
assert_eq(profile.name, "Ada")
assert_eq(profile["city"], "Seoul")
assert_eq(profile.name + " Lovelace", "Ada Lovelace")
assert_eq("City: " + profile["city"], "City: Seoul")
assert_eq("Score: " + meta.score, "Score: 9")
assert_eq(greeting, "Hello Ada")
assert_eq(greeting + "!", "Hello Ada!")
assert_eq(nested_function_result, "call ok")
assert_eq(nested_function_type, "string")
assert_eq(nested_method_result, "Nested method string method-ok")
assert_eq(nested_method_type, "string")
assert_eq(if_local_method_result, "If method string if-ok")
assert_eq(if_local_method_type, "string")
assert_eq(match_local_method_result, "Match method string match-ok")
assert_eq(match_local_method_type, "string")
assert_eq(score_label, "Score 9")
assert_eq(optional_dot_nil_value, nil)
assert_eq(optional_nested_nil_value, nil)
assert_eq(optional_existing_value, "Ada")
assert_eq(nil_numeric_index_value, nil)
assert_eq(nil_string_index_value, nil)
assert_eq(type(nil_index_source[0]), "nil")
assert_eq(to_str(nil_index_source["name"]), "nil")
assert_eq(optional_coalesce_nil_value, "anon")
assert_eq(optional_coalesce_nested_value, "anon")
assert_eq(optional_coalesce_existing_value, "Ada")
assert_eq(optional_coalesce_interp_label, "Maybe anon Existing Ada")
assert_eq(optional_coalesce_call_label, "CoalesceCalls string 4 true anon")
assert_eq(to_str(optional_items_value), "[\"fallback\", \"items\"]")
assert_eq(to_str(optional_existing_items_value), "[\"Ada\", \"Grace\"]")
assert_eq(optional_items_label, "Items [\"fallback\", \"items\"] [\"Ada\", \"Grace\"] 2 true")
assert_eq(optional_false_value, false)
assert_eq(optional_zero_value, 0)
assert_eq(optional_false_fallback_value, true)
assert_eq(optional_zero_fallback_value, 99)
assert_eq(optional_falsey_label, "Falsey false 0 true 99")
assert_eq(false ?? true, false)
assert_eq(0 ?? 99, 0)
assert_eq(nil ?? "fallback", "fallback")
assert_eq(direct_falsey_label, "DirectFalsey false 0 fallback")
assert_eq(direct_falsey_call_label, "DirectCalls false 0 string")
assert_eq(type(1 ?? "fallback"), "number")
assert_eq(to_str(1 ?? "fallback"), "1")
assert_eq(type(nil ?? ["fallback"]), "array")
assert_eq(to_str(nil ?? ["fallback"]), "[\"fallback\"]")
assert_eq(mixed_coalesce_label, "MixedCoalesce number 1 array [\"fallback\"]")
assert_eq(coalesce_short_value, "left")
assert_eq(coalesce_effect_after_short, 0)
assert_eq(coalesce_nil_value, "called")
assert_eq(coalesce_effect_after_nil, 1)
assert_eq(coalesce_short_label, "ShortCircuit left 0 called 1")
assert(flags.active == true)
assert(flags["active"] == true)
assert(flags.missing == nil)
assert_eq(flags.active, true)
assert_eq(flags["active"], true)
assert_eq(flags.missing, nil)
assert_ne(flags.active, false)
assert_eq("Active: " + flags.active, "Active: true")
assert_eq("Missing: " + flags.missing, "Missing: nil")
assert_eq(flag_label, "Active true Missing nil")
assert(names[0] == "Ada")
assert_eq(names[1], "Grace")
assert_eq(names[0] + " Lovelace", "Ada Lovelace")
assert_eq(names[dynamic_idx], "Grace")
assert_eq("Dynamic " + names[dynamic_idx], "Dynamic Grace")
assert_eq(dynamic_name_label, "Dynamic Grace")
assert_eq(push_len, 3)
assert_eq(push_tail, 3)
assert_eq(push_label, "Push 3 3")
assert_eq(pop_tail, 6)
assert_eq(pop_len, 2)
assert_eq(pop_name, "Grace")
assert_eq(pop_name_len, 1)
assert_eq(pop_flag, false)
assert_eq(pop_flag_len, 1)
assert_eq(pop_label, "Pop 6 2 Grace 1 false 1")
assert_eq(reverse_values_label, "3:2:1")
assert_eq(reverse_result_label, "3:2:1")
assert_eq(reverse_names_label, "Grace/Ada")
assert_eq(reverse_flags_label, "false|true")
assert_eq(reverse_label, "Reverse 3:2:1 3:2:1 Grace/Ada false|true")
assert_eq(sort_values_label, "1,2,3")
assert_eq(sort_result_label, "1,2,3")
assert_eq(sort_module_label, "-1:4:9")
assert_eq(sort_label, "Sort 1,2,3 1,2,3 -1:4:9")
assert_eq(repeat_values_label, "7,7,7")
assert_eq(repeat_names_label, "haha")
assert_eq(repeat_flags_label, "true|true")
assert_eq(repeat_empty_len, 0)
assert_eq(repeat_label, "Repeat 7,7,7 haha true|true 0")
assert_eq(unique_values_label, "1,2")
assert_eq(unique_module_label, "3|1|2")
assert_eq(unique_method_label, "4:5")
assert_eq(unique_label, "Unique 1,2 3|1|2 4:5")
assert_eq(set_union_values_label, "1,2,3,4")
assert_eq(set_union_module_label, "5|6|7")
assert_eq(set_union_single_label, "9:8")
assert_eq(set_union_label, "SetUnion 1,2,3,4 5|6|7 9:8")
assert_eq(set_intersection_values_label, "2,3")
assert_eq(set_intersection_module_label, "5|7")
assert_eq(set_difference_values_label, "1,3")
assert_eq(set_difference_module_label, "6|7")
assert_eq(set_symdiff_values_label, "1,4,5")
assert_eq(set_symdiff_module_label, "7|9")
assert_eq(set_symdiff_alias_label, "10:12")
assert_eq(set_subset_true, true)
assert_eq(set_subset_false, false)
assert_eq(set_subset_alias, true)
assert_eq(set_superset_true, true)
assert_eq(set_superset_false, false)
assert_eq(set_superset_alias, true)
assert_eq(set_interdiff_label, "SetOps 2,3 5|7 1,3 6|7 1,4,5 7|9 10:12 true false true true false true")
assert_eq(clone_source_label, "8,9")
assert_eq(clone_result_label, "4,9")
assert_eq(clone_copy_label, "8,5")
assert_eq(clone_label, "Clone 8,9 4,9 8,5")
assert_eq(concat_left_label, "1,2")
assert_eq(concat_right_label, "3,4")
assert_eq(concat_result_label, "1,9,3,4,5")
assert_eq(concat_names_label, "ABC")
assert_eq(concat_label, "Concat 1,2 3,4 1,9,3,4,5 ABC")
assert_eq(chunk_count, 3)
assert_eq(chunk_first_len, 2)
assert_eq(chunk_second_len, 2)
assert_eq(chunk_last_len, 1)
assert_eq(chunk_first_item, 2)
assert_eq(chunk_second_item, 3)
assert_eq(chunk_last_item, 5)
assert_eq(chunk_empty_len, 0)
assert_eq(chunk_label, "Chunk 3 2 2 1 2 3 5 0")
assert_eq(zip_count, 2)
assert_eq(zip_first_len, 2)
assert_eq(zip_second_len, 2)
assert_eq(zip_first_left, 1)
assert_eq(zip_first_right, 3)
assert_eq(zip_second_left, 2)
assert_eq(zip_second_right, 4)
assert_eq(zip_empty_len, 0)
assert_eq(zip_label, "Zip 2 2 2 1 3 2 4 0")
assert_eq(flatten_chunks_label, "1,2,3,4,5")
assert_eq(flatten_zip_label, "1:3:2:4")
assert_eq(flatten_method_label, "6|7|8")
assert_eq(flatten_literal_label, "9/10/11")
assert_eq(flatten_label, "Flatten 1,2,3,4,5 1:3:2:4 6|7|8 9/10/11")
assert_eq(dict_api_keys_label, "alpha|beta")
assert_eq(dict_api_values_label, "1:2")
assert_eq(dict_api_direct_keys_label, "gamma/omega")
assert_eq(dict_api_direct_values_label, "3,4")
assert_eq(dict_api_items_len, 2)
assert_eq(dict_api_item_first_len, 2)
assert_eq(dict_api_item_second_len, 2)
assert_eq(dict_api_direct_item_len, 2)
assert_eq(dict_api_item_first_value, 1)
assert_eq(dict_api_item_second_value, 2)
assert_eq(dict_api_direct_item_value, 8)
assert_eq(dict_api_label, "DictApi alpha|beta 1:2 gamma/omega 3,4 2 2 2 2 1 2 8")
assert_eq(dict_api_picked_keys_label, "gamma|alpha")
assert_eq(dict_api_picked_values_label, "3:1")
assert_eq(dict_api_picked_gamma, 3)
assert_eq(dict_api_picked_alpha, 1)
assert_eq(dict_api_picked_method_beta, 2)
assert_eq(dict_api_direct_pick_right, 9)
assert_eq(dict_api_pick_label, "DictPick gamma|alpha 3:1 3 1 2 9")
assert_eq(dict_api_omitted_keys_label, "alpha|gamma")
assert_eq(dict_api_omitted_values_label, "1:3")
assert_eq(dict_api_omitted_alpha, 1)
assert_eq(dict_api_omitted_gamma, 3)
assert_eq(dict_api_omitted_method_beta, 2)
assert_eq(dict_api_direct_omit_right, 9)
assert_eq(dict_api_omit_label, "DictOmit alpha|gamma 1:3 1 3 2 9")
assert_eq(dict_api_merged_keys_label, "alpha|beta|gamma|delta")
assert_eq(dict_api_merged_values_label, "1:20:30:4")
assert_eq(dict_api_merged_alpha, 1)
assert_eq(dict_api_merged_beta, 20)
assert_eq(dict_api_merged_gamma, 30)
assert_eq(dict_api_merged_delta, 4)
assert_eq(dict_api_merged_method_beta, 22)
assert_eq(dict_api_merged_method_omega, 5)
assert_eq(dict_api_direct_merge_left, 80)
assert_eq(dict_api_direct_merge_right, 9)
assert_eq(dict_api_merge_label, "DictMerge alpha|beta|gamma|delta 1:20:30:4 1 20 30 4 22 5 80 9")
assert_eq(dict_path_hp, 42)
assert_eq(dict_path_name, "Ari")
assert_eq(dict_path_name_method, "Ari")
assert_eq(dict_path_missing, nil)
assert_eq(dict_path_label, "DictPath 42 Ari Ari nil")
assert_eq(json_path_hp, 42)
assert_eq(json_path_name, "Ari")
assert_eq(json_path_name_method, "Ari")
assert_eq(json_has_hp, true)
assert_eq(json_has_missing, false)
assert_eq(json_path_label, "JsonPath 42 Ari Ari true false")
assert_eq(clear_result, nil)
assert_eq(clear_len, 0)
assert_eq(clear_label, "Clear nil 0")
assert_eq(insert_label, "1,2,3")
assert_eq(insert_front_label, "1,2,3")
assert_eq(insert_tail_label, "1,2,3")
assert_eq(removed_mid, 5)
assert_eq(remove_after_mid_label, "4:6")
assert_eq(removed_tail, 6)
assert_eq(remove_after_tail_label, "4")
assert_eq(removed_missing, nil)
assert_eq(remove_missing_len, 1)
assert_eq(remove_label, "InsertRemove 1,2,3 1,2,3 1,2,3 5 4:6 6 4 nil 1")
assert(states[0] == true)
assert_eq(states[0], true)
assert_eq(states[2], nil)
assert_ne(states[1], true)
assert_eq(bools[dynamic_idx], false)
assert_eq("Bool " + bools[dynamic_idx], "Bool false")
assert_eq(dynamic_bool_label, "Bool false")
assert_eq(contains(search_text, "wasm"), true)
assert_eq(contains(search_text, "python"), false)
assert_eq(startsWith(search_text, "su"), true)
assert_eq(startsWith(search_text, "ra"), false)
assert_eq(endsWith(search_text, "asm"), true)
assert_eq(endsWith(search_text, "su"), false)
assert_eq(string.contains(search_text, "wasm"), true)
assert_eq(string.starts_with(search_text, "su"), true)
assert_eq(string.ends_with(search_text, "asm"), true)
assert_eq(ast_string_search_label, "Search true true true false")
assert_eq(indexOf(search_text, "wasm"), 5)
assert_eq(indexOf(search_text, "python"), -1)
assert_eq(indexOf(search_text, ""), 0)
assert_eq(string.index_of(search_text, "sura"), 0)
assert_eq(string.indexOf(search_text, "wasm"), 5)
assert_eq(search_text.index_of("asm"), 6)
assert_eq(search_text.indexOf("ra"), 2)
assert_eq(ast_string_index_of_label, "Index 5 -1 0 6")
assert_eq(upper(search_text), "SURA WASM")
assert_eq(lower("SURA WASM"), "sura wasm")
assert_eq(trim(transform_text), "Sura WASM")
assert_eq(string.upper("ok"), "OK")
assert_eq(string.lower("AI"), "ai")
assert_eq(string.trim("  wasm  "), "wasm")
assert_eq(search_text.upper(), "SURA WASM")
assert_eq("SURA".lower(), "sura")
assert_eq("  core  ".trim(), "core")
assert_eq(search_text.contains("wasm"), true)
assert_eq(search_text.starts_with("su"), true)
assert_eq(search_text.ends_with("asm"), true)
assert_eq(ast_string_transform_label, "Transform SURA WASM sura wasm Sura WASM OK SURA WASM sura core")
assert_eq(replace(search_text, "wasm", "WASM"), "sura WASM")
assert_eq(replace(search_text, "", "x"), "sura wasm")
assert_eq(string.replace("sura wasm sura", "sura", "SURA"), "SURA wasm SURA")
assert_eq("banana".replace("na", "NA"), "baNANA")
assert_eq(ast_string_replace_label, "Replace sura WASM sura wasm SURA wasm SURA baNANA")
assert_eq(substring(search_text, 5), "wasm")
assert_eq(slice(search_text, -4), "wasm")
assert_eq(slice(search_text, 0, 4), "sura")
assert_eq(string.substring(search_text, 0, 4), "sura")
assert_eq(string.sub(search_text, 5), "wasm")
assert_eq(search_text.sub(0, 4), "sura")
assert_eq(search_text.slice(5), "wasm")
assert_eq(search_text.substring(5, 9), "wasm")
assert_eq(ast_string_slice_label, "Slice wasm wasm sura wasm sura wasm wasm")
assert_eq(string_repeat("ha", 3), "hahaha")
assert_eq(string.repeat("go", 2), "gogo")
assert_eq("na".repeat(4), "nananana")
assert_eq(string.repeat("z", 0), "")
assert_eq(ast_string_repeat_label, "Repeat hahaha gogo nananana ")
assert_eq(string_pad_left("7", 3, "0"), "007")
assert_eq(string.pad_right("go", 4, "!"), "go!!")
assert_eq("x".pad_left(3, "_"), "__x")
assert_eq("ok".pad_right(2, "?"), "ok")
assert_eq(string_pad_left("x", 3), "  x")
assert_eq(ast_string_pad_label, "Pad 007 go!! __x ok")
assert_eq(split_parts.len(), 3)
assert_eq(split_parts[0], "sura")
assert_eq(split_parts[1], "wasm")
assert_eq(split_parts[2], "lang")
assert_eq(split_parts.join("/"), "sura/wasm/lang")
assert_eq(split_module_parts.len(), 3)
assert_eq(split_module_parts[2], "green")
assert_eq(split_empty_parts.len(), 1)
assert_eq(split_empty_parts[0], "solo")
assert_eq(ast_string_split_label, "Split 3 sura wasm sura/wasm/lang green 1 solo")
assert_eq(line_parts.len(), 3)
assert_eq(line_parts[0], "top")
assert_eq(line_parts[2], "end")
assert_eq(line_parts.join("|"), "top|mid|end")
assert_eq(line_module_parts[1], "green")
assert_eq(line_receiver_parts.len(), 2)
assert_eq(line_receiver_parts.join("/"), "left/right")
assert_eq(empty_line_parts.len(), 0)
assert_eq(ast_string_lines_label, "Lines 3 top end top|mid|end green 2 left/right 0")
assert_eq(word_parts.len(), 3)
assert_eq(word_parts[0], "alpha")
assert_eq(word_parts[2], "gamma")
assert_eq(word_parts.join("|"), "alpha|beta|gamma")
assert_eq(word_module_parts[1], "green")
assert_eq(word_receiver_parts.len(), 3)
assert_eq(word_receiver_parts.join("/"), "left/right/center")
assert_eq(empty_word_parts.len(), 0)
assert_eq(ast_string_words_label, "Words 3 alpha gamma alpha|beta|gamma green 3 left/right/center 0")
assert_eq(chunk_parts.len(), 3)
assert_eq(chunk_parts[0], "abc")
assert_eq(chunk_parts[1], "cde")
assert_eq(chunk_parts[2], "efg")
assert_eq(chunk_parts.join("|"), "abc|cde|efg")
assert_eq(chunk_alias_parts.len(), 1)
assert_eq(chunk_alias_parts[0], "tiny")
assert_eq(chunk_module_parts.join("/"), "he/ll/o")
assert_eq(chunk_receiver_parts.join("-"), "su-ra")
assert_eq(empty_chunk_parts.len(), 0)
assert_eq(ast_text_chunks_label, "Chunks 3 abc cde efg abc|cde|efg 1 tiny he/ll/o su-ra 0")
assert_eq("State: " + states[0], "State: true")
assert_eq("Missing: " + states[2], "Missing: nil")
assert_eq(array_label, "Name Ada Active true Missing nil")
assert_eq(profile.name == names[0], true)
assert_eq("Compare direct: " + (profile.name == names[0]), "Compare direct: true")
assert_eq(compare_label, "Compare true Low true High false")
assert_eq(profile[profile_key_lookup], "Seoul")
assert_eq("Dynamic city " + profile[profile_key_lookup], "Dynamic city Seoul")
assert_eq(profile_dynamic_label, "Profile Seoul")
assert_eq(profile_has_name, true)
assert_eq(profile_has_missing, false)
assert_eq(profile_contains_city, true)
assert_eq(dict_module_has_name, true)
assert_eq(dict_module_contains_missing, false)
assert_eq(dynamic_profile_has_missing, true)
assert_eq(dynamic_profile_contains_name, true)
assert_eq(dict_has_label, "DictHas true false true true false true true")
assert_eq(join_names_label, "Ada|Grace")
assert_eq(join_scores_label, "1,2,3")
assert_eq(join_flags_label, "true/false")
assert_eq(join_nils_label, "nil-nil")
assert_eq(join_label, "Join Ada|Grace 1,2,3 true/false nil-nil")
assert_eq(slice_scores_label, "2:3")
assert_eq(slice_names_label, "Ada")
assert_eq(slice_flags_label, "true")
assert_eq(slice_nils_label, "nil")
assert_eq(slice_label, "Slice 2:3 Ada true nil")
profile[profile_update_key] is "Paris"
assert_eq(profile["city"], "Paris")
assert_eq(profile[profile_key_lookup], "Paris")
assert_eq(type(profile), "dict")
assert_eq(type(names), "array")
assert_eq(type(flags.active), "bool")
assert_eq(type(flags.missing), "nil")
assert_eq(type(score), "number")
assert_eq(type(point), "instance")
assert_eq(type(block_double_ast), "function")
assert_eq(type(function_alias), "function")
assert_eq(type(function_handlers[0]), "function")
assert_eq(type(function_handlers[function_idx]), "function")
assert_eq(type(function_map.primary), "function")
assert_eq(type(function_map[function_key_lookup]), "function")
assert_eq(type(function_choice), "function")
assert_eq(type(function_pick_call), "function")
assert_eq(type(function_pick_true), "function")
assert_eq(type(function_match_pick), "function")
assert_eq(type(function_passed), "function")
assert_eq(function_param_call, 24)
assert_eq(function_param_call_label, "ParamFn function <Func block_double_ast> 26")
assert_eq(dynamic_function_param_call_a, 12)
assert_eq(dynamic_function_param_call_b, 18)
assert_eq(dynamic_local_function_call_a, 10)
assert_eq(dynamic_local_function_call_b, 15)
assert_eq(dynamic_zero_function_call_a, 11)
assert_eq(dynamic_zero_function_call_b, 17)
assert_eq(dynamic_zero_function_call_label, "DynamicZeroFunction 11 17")
assert_eq(dynamic_binary_function_call_a, 7)
assert_eq(dynamic_binary_function_call_b, 12)
assert_eq(dynamic_binary_function_call_label, "DynamicBinaryFunction 7 12")
assert_eq(dynamic_binary_dict_function_call_a, 11)
assert_eq(dynamic_binary_dict_function_call_b, 30)
assert_eq(dynamic_binary_dict_function_call_label, "DynamicBinaryDictFunction 11 30")
assert_eq(dynamic_binary_array_function_call_a, 15)
assert_eq(dynamic_binary_array_function_call_b, 56)
assert_eq(dynamic_binary_array_function_call_label, "DynamicBinaryArrayFunction 15 56")
assert_eq(foreach_binary_function_call_a, 11)
assert_eq(foreach_binary_function_call_b, 18)
assert_eq(foreach_binary_function_call_label, "ForeachBinaryFunction 11 18")
assert_eq(dynamic_binary_param_function_call_a, 19)
assert_eq(dynamic_binary_param_function_call_b, 90)
assert_eq(dynamic_binary_param_function_call_label, "DynamicBinaryParamFunction 19 90")
assert_eq(dynamic_triple_function_call_a, 9)
assert_eq(dynamic_triple_function_call_b, 10)
assert_eq(dynamic_triple_function_call_label, "DynamicTripleFunction 9 10")
assert_eq(dynamic_quad_function_call_a, 10)
assert_eq(dynamic_quad_function_call_b, 9)
assert_eq(dynamic_quad_function_call_label, "DynamicQuadFunction 10 9")
assert_eq(dynamic_five_function_call_a, 15)
assert_eq(dynamic_five_function_call_b, 14)
assert_eq(dynamic_five_function_call_label, "DynamicFiveFunction 15 14")
assert_eq(dynamic_eight_function_call_a, 36)
assert_eq(dynamic_eight_function_call_b, 35)
assert_eq(dynamic_eight_function_call_label, "DynamicEightFunction 36 35")
assert_eq(dynamic_string_function_call_a, "Ax")
assert_eq(dynamic_string_function_call_b, "Bx")
assert_eq(dynamic_string_function_call_label, "DynamicStringFunction Ax Bx")
assert_eq(dynamic_bool_function_call_a, true)
assert_eq(dynamic_bool_function_call_b, false)
assert_eq(dynamic_bool_function_call_label, "DynamicBoolFunction true false false")
assert_eq(length(dynamic_array_function_call_a), 2)
assert_eq(dynamic_array_function_call_a[1], "a")
assert_eq(dynamic_array_function_call_b[1], "b")
assert_eq(dynamic_array_function_call_label, "DynamicArrayFunction 2 [8, \"b\"]")
assert_eq(type(dynamic_nil_function_call_a), "nil")
assert_eq(dynamic_nil_function_call_label, "DynamicNilFunction nil false")
assert_eq(length(dynamic_dict_return_function_call_a), 2)
assert_eq(dynamic_dict_return_function_call_a.name, "a")
assert_eq(dynamic_dict_return_function_call_b.score, 8)
assert_eq(dynamic_dict_return_function_call_label, "DynamicDictReturnFunction 2 b")
assert_eq(dynamic_function_return_function_call, 12)
assert_eq(dynamic_function_return_function_call_label, "DynamicFunctionReturnFunction function true 12")
assert_eq(dynamic_dict_lookup_dict_return_function_b.name, "b")
assert_eq(dynamic_dict_lookup_dict_return_function_label, "DynamicDictLookupDictReturnFunction 2 8 b")
assert_eq(dynamic_array_lookup_function_return_call, 12)
assert_eq(dynamic_array_lookup_function_return_label, "DynamicArrayLookupFunctionReturn function true 12")
assert_eq(dynamic_dict_function_call_a, 12)
assert_eq(dynamic_dict_function_call_b, 18)
assert_eq(direct_dynamic_dict_function_call_label, "DirectDynamicDictFn 21")
assert_eq(type_label, "Types dict array bool nil number instance")
assert_eq(function_type_label, "Functions function function true")
assert_eq(local_function_value_label, "LocalFn function <Func local_inner_ast> 12 13 true function")
assert_eq(inline_function_value_label, "InlineFn function true <Func <lambda>> function true <Func <lambda>>")
assert_eq(direct_inline_function_value_label, "DirectInlineFn function true <Func <lambda>>")
assert_eq(returned_inline_function_label, "ReturnInlineFn function true <Func returned_inline_function_value_ast_return> 27")
assert_eq(returned_param_function_capture_label, "ReturnedFunctionCapture function true 18")
captured_inline_expected_profile is to_str({"name": "Sura", "city": "Seoul"})
captured_inline_expected_label is "CapturedInlineFn 17 12 23 SuraLang sura SURA sura true true true 2 sURa ur ra Math -2 12 8 4 9 3 Calls SURA true ra Direct 1 true true true Lang Match Collection 12 Sura Seoul 2 dict true 22 11 10 true 0 [2, 4, 6] [10, 12] " + captured_inline_expected_profile + " Snapshot 12 Sura [10, 12] " + captured_inline_expected_profile + " function <Func returned_captured_inline_function_ast_return> 20 12 13 13 13 13 13 13 13 18 24 13 24 13 13 13 13"
assert_eq(captured_inline_function_label, captured_inline_expected_label)
assert_eq(control_flow_function_alias_label, "FlowFn 18 20 22")
assert_eq(function_lookup_label, "Function lookup function function true")
assert_eq(function_param_label, "Func function true")
assert_eq(type(function_holder.handler), "function")
assert_eq(type(function_holder.handler_value()), "function")
assert_eq(type(function_holder.choose_handler(true)), "function")
assert_eq(type(function_holder.choose_handler(false)), "function")
assert_eq(type(function_child.inherited_handler()), "function")
assert_eq(function_holder.handler_type(), "function")
assert_eq(function_holder.handler_truth(), true)
assert_eq(function_holder_label, "Handler function true")
assert_eq(function_holder_call, 28)
assert_eq(function_holder_alt_call, 21)
assert_eq(function_holder_dynamic_dict_call_label, "MethodDynamicDictFn 21")
assert_eq(function_holder_dynamic_dict_profile_label, "MethodDynamicDictProfile 2 8 b")
assert_eq(super_object_param_type_label, "SuperObjParamType string")
assert_eq(super_object_param_array_len_label, "SuperObjArrayLen 2")
assert_eq(function_holder_return_call, 30)
assert_eq(function_pick_true_call, 32)
assert_eq(function_holder_chosen_call, 32)
assert_eq(function_match_pick_call, 34)
assert_eq(function_holder_match_call, 34)
assert_eq(function_holder_lookup_return_call, 12)
assert_eq(function_holder_lookup_return_label, "MethodArrayLookupFunctionReturn function true 12")
assert_eq(binary_function_holder_call, 11)
assert_eq(binary_function_holder_alt_call, 30)
assert_eq(binary_function_holder_chosen_call, 30)
assert_eq(binary_function_holder_dict_call_label, "MethodBinaryDictFn 11 30")
assert_eq(binary_function_child_call, 30)
assert_eq(binary_function_child_direct_call, 30)
assert_eq(function_child_label, "Inherited function true")
assert_eq(function_child_inline_call, 30)
assert_eq(function_child_inline_label, "SuperInline function <Func AstFunctionHolder_inline_handler_value_return> 31")
assert_eq(function_child_inline_choice_label, "SuperInlineChoice function <Func <lambda>> 33")
assert_eq(to_str(block_double_ast), "<Func block_double_ast>")
assert_eq(function_to_str_label, "Fn <Func block_double_ast>")
assert_eq(to_str(function_alias), "<Func block_double_ast>")
assert_eq(function_alias_to_str_label, "Alias <Func block_double_ast>")
assert_eq(to_str(function_handlers[0]), "<Func block_double_ast>")
assert_eq(to_str(function_map["primary"]), "<Func block_double_ast>")
assert_eq(function_index_to_str_label, "FnIndex <Func block_double_ast> <Func block_double_ast>")
assert_eq(to_str(function_map[function_key_lookup]), "<Func block_double_ast>")
assert_eq(function_dynamic_to_str_label, "FnDynamic <Func block_double_ast>")
assert_eq(to_str(function_handlers[function_idx]), "<Func format_pair_ast>")
assert_eq(function_dynamic_index_to_str_label, "FnDynIndex <Func format_pair_ast>")
assert_eq(function_foreach_label, "functionfunction")
assert_eq(function_foreach_dict_label, "functionfunction")
assert_eq(object_foreach_label, "botbot")
assert_eq(object_foreach_dict_label, "botbot")
assert_eq(object_foreach_return_call_label, "bot")
assert_eq(object_foreach_dict_return_call_label, "bot")
assert_eq(object_param_method_label, "bot!")
assert_eq(object_param_field_label, "bot?")
assert_eq(object_param_index_label, "bot.")
assert_eq(object_function_param_field_label, "bot#")
assert_eq(object_function_param_index_label, "bot;")
assert_eq(object_ctor_field_method_label, "bot~")
assert_eq(object_ctor_field_label, "bot%")
assert_eq(object_ctor_index_label, "bot&")
assert_eq(object_super_ctor_method_label, "bot+")
assert_eq(object_super_ctor_child_method_label, "bot=")
assert_eq(object_super_ctor_field_label, "bot:")
assert_eq(object_super_ctor_index_label, "bot,")
assert_eq(function_truth_score, 15)
assert_eq(not function_alias, false)
assert_type(profile, "dict")
assert_type(names, "array")
assert_type(flags.active, "bool")
assert_type(flags.missing, "nil")
assert_type(score, "number")
assert_type(point, "instance")
assert_type(block_double_ast, "function")
assert_type(function_alias, "function")
assert_type(function_choice, "function")
assert_type(function_pick_call, "function")
assert_type(function_pick_true, "function")
assert_type(function_match_pick, "function")
assert_type(function_passed, "function")
assert_type(function_holder.handler, "function")
assert_type(function_holder.handler_value(), "function")
assert_type(function_child.inherited_handler(), "function")
assert_type(function_holder_label, "string")
assert_type(function_child_label, "string")
assert_type(function_foreach_label, "string")
assert_type(function_foreach_dict_label, "string")
assert_type(type_label, "string")
assert_type(function_type_label, "string")
assert_type(function_lookup_label, "string")
assert_type(function_param_label, "string")
assert(block_double_ast)
assert(function_alias)
assert(function_handlers[function_idx])
assert(function_map[function_key_lookup])
assert(function_holder.handler)
assert(function_holder.handler_value())
assert(function_child.inherited_handler())
assert_eq(to_bool(block_double_ast), true)
assert_eq(block_double_ast, function_alias)
assert_eq(function_handlers[0], block_double_ast)
assert_eq(function_map[function_key_lookup], block_double_ast)
assert_eq(function_choice, block_double_ast)
assert_eq(function_pick_true, block_double_ast)
assert_eq(function_match_pick, block_double_ast)
assert_eq(function_passed, block_double_ast)
assert_eq(function_holder.handler, block_double_ast)
assert_eq(function_holder.handler_value(), block_double_ast)
assert_eq(function_holder.choose_handler(true), block_double_ast)
assert_eq(function_child.inherited_handler(), block_double_ast)
assert_eq(function_holder.same_handler(block_double_ast), true)
assert_ne(block_double_ast, format_pair_ast)
assert_ne(function_handlers[function_idx], block_double_ast)
assert_ne(function_pick_call, block_double_ast)
assert_ne(function_holder.choose_handler(false), block_double_ast)
function_map[function_update_key] is block_double_ast
assert_eq(type(function_map[function_update_key]), "function")
assert_eq(function_map[function_update_key], block_double_ast)
function_holder.handler is format_pair_ast
assert_eq(type(function_holder.handler), "function")
assert_eq(type(function_holder.handler_value()), "function")
assert_eq(function_holder.handler, format_pair_ast)
assert_eq(function_holder.handler_value(), format_pair_ast)
assert_eq(function_holder.same_handler(block_double_ast), false)
assert_eq(function_holder.same_handler(format_pair_ast), true)
assert_eq(length(profile), 2)
assert_eq(length(names), 2)
assert_eq(length(profile["city"]), 5)
assert_eq(length(type(profile)), 4)
assert_eq(len_label, "Lengths 2 2 4")
assert_eq(length(true ? [1, 2, 3] : {name: profile.name}), 3)
assert_eq(length(false ? [1, 2, 3] : {name: profile.name}), 1)
assert_eq(length(true ? "yes" : [1, 2]), 3)
assert_eq(length(false ? "yes" : [1, 2]), 2)
assert_eq(len_mixed_ternary_label, "MixedLen 3 1 3 2")
assert_len(profile, 2)
assert_len(names, 2)
assert_len(profile["city"], 5)
assert_len(type(profile), 4)
assert_eq(to_str(score), "16")
assert_eq(to_str(flags.active), "true")
assert_eq(to_str(flags.missing), "nil")
assert_eq(to_str(profile.name), "Ada")
assert_eq(str_label, "Strings 16 true nil Ada")
assert_eq(to_str([score, profile.name, flags.active, flags.missing]), "[16, \"Ada\", true, nil]")
assert_eq(literal_array_to_str_label, "Literal [16, \"Ada\", true, nil]")
expected_name_dict_string is to_str({name: "Ada"})
assert_eq(to_str({name: profile.name}), expected_name_dict_string)
assert_eq(literal_dict_to_str_label, "Dict " + expected_name_dict_string)
assert_eq(to_str([score, "ok", flags.active, flags.missing]), "[16, \"ok\", true, nil]")
expected_runtime_dict_string is to_str({score: 16, active: true, missing: nil})
assert_eq(to_str({score: score, active: flags.active, missing: flags.missing}), expected_runtime_dict_string)
assert_eq(runtime_collection_to_str_label, "RuntimeCollection [16, \"ok\", true, nil] " + expected_runtime_dict_string)
assert_eq(to_str(true ? [score, profile.name] : [0, "No"]), "[16, \"Ada\"]")
assert_eq(ternary_literal_to_str_label, "Choice [16, \"Ada\"]")
assert_eq(to_str(values), "[1, 2, 3]")
assert_eq(to_str(names), "[\"Ada\", \"Grace\"]")
assert_eq(to_str(bools), "[true, false]")
assert_eq(array_var_to_str_label, "ArrayVar [1, 2, 3] [\"Ada\", \"Grace\"] [true, false]")
assert_eq(to_str(mixed_values), "[16, \"Ada\", true, nil]")
assert_eq(mixed_array_var_to_str_label, "MixedArrayVar [16, \"Ada\", true, nil]")
assert_eq(to_str(mixed_values[mixed_to_str_idx]), "true")
assert_eq(mixed_array_index_to_str_label, "MixedIndex true")
assert_eq(type(mixed_values[mixed_type_idx]), "bool")
assert_eq(to_bool(mixed_values[mixed_bool_idx]), true)
assert_eq(length(mixed_values[mixed_len_idx]), 3)
assert_eq(mixed_array_index_runtime_label, "MixedRuntime bool true 3")
assert_eq(value_runtime_array_index_label, "ValueRuntimeIndex string one nil nil")
assert_eq(type(mixed_values_from_func), "array")
assert_eq(type(mixed_values_from_func[0]), "number")
assert_eq(type(mixed_values_from_func[1]), "string")
assert_eq(type(mixed_values_from_func[2]), "bool")
assert_eq(type(mixed_values_from_func[3]), "nil")
assert_eq(to_str(mixed_values_from_func[0]), "32")
assert_eq(to_str(mixed_values_from_func[1]), "ok")
assert_eq(to_str(mixed_values_from_func[2]), "true")
assert_eq(to_str(mixed_values_from_func[3]), "nil")
assert_eq(mixed_function_return_array_label, "MixedReturn 32 ok true nil")
assert_eq(type(make_mixed_values_ast()[1]), "string")
assert_eq(length(make_mixed_values_ast()[1]), 2)
assert_eq(to_str(make_mixed_values_ast()[2]), "true")
assert_eq(to_bool(make_mixed_values_ast()[2]), true)
assert_eq(direct_mixed_function_return_array_label, "DirectMixedReturn number ok true nil")
assert_eq(direct_mixed_function_return_array_to_str_label, "DirectMixedArrayString [32, \"ok\", true, nil]")
assert_eq(direct_mixed_function_return_array_interp_label, "DirectMixedArrayInterp [32, \"ok\", true, nil]")
assert_eq(direct_mixed_function_return_array_concat_label, "DirectMixedArrayConcat [32, \"ok\", true, nil]")
assert_eq(direct_mixed_function_return_collection_runtime_label, "DirectMixedCollectionRuntime 4 true 4 true 0 false 0 false")
assert_eq(param_return_collection_runtime_label, "ParamReturnCollectionRuntime 3 true 0 false 2 true 0 false")
expected_score_bonus_dict_string is to_str({score: 9, bonus: 3})
expected_empty_dict_string is to_str({})
assert_eq(function_return_choice_collection_to_str_label, "FunctionChoiceCollectionString [1, 2, 3] [] " + expected_score_bonus_dict_string + " " + expected_empty_dict_string)
assert_eq(function_return_local_choice_collection_to_str_label, "FunctionLocalChoiceCollectionString [1, 2, 3] [] " + expected_score_bonus_dict_string + " " + expected_empty_dict_string)
assert_eq(function_return_choice_access_label, "FunctionChoiceAccess 1 number number 3")
assert_eq(function_return_local_choice_access_label, "FunctionLocalChoiceAccess 1 number number 3")
assert_eq(function_return_alias_collection_to_str_label, "FunctionAliasCollectionString [1, 2, 3] [] " + expected_score_bonus_dict_string + " " + expected_empty_dict_string)
assert_eq(function_return_alias_access_label, "FunctionAliasAccess 1 number 3")
assert_eq(function_return_local_scope_alias_label, "FunctionLocalScopeAlias [1, 2, 3] [] " + expected_score_bonus_dict_string + " " + expected_empty_dict_string + " 3")
assert_eq(method_return_collection_to_str_label, "MethodCollectionString [1, 2, 3] " + expected_score_bonus_dict_string)
assert_eq(method_return_choice_collection_to_str_label, "MethodChoiceCollectionString [1, 2, 3] [] " + expected_score_bonus_dict_string + " " + expected_empty_dict_string)
assert_eq(method_return_local_choice_collection_to_str_label, "MethodLocalChoiceCollectionString [1, 2, 3] [] " + expected_score_bonus_dict_string + " " + expected_empty_dict_string)
assert_eq(method_return_choice_access_label, "MethodChoiceAccess 1 number number 3")
assert_eq(method_return_local_choice_access_label, "MethodLocalChoiceAccess 1 number number 3")
assert_eq(constructor_param_collection_label, "ConstructorParamCollection [1, 2, 3] 3")
assert_eq(super_constructor_param_collection_label, "SuperConstructorParamCollection [1, 2, 3] 3")
assert_eq(super_method_param_collection_label, "SuperMethodParamCollection [1, 2, 3] 3")
assert_eq(super_method_return_collection_label, "SuperMethodReturnCollection [1, 2, 3] 3")
assert_eq(super_method_update_label, "SuperMethodUpdate 11")
assert_eq(super_method_dynamic_update_label, "SuperMethodDynamicUpdate 12")
assert_eq(super_call_key_string_label, "SuperMethodCallKeyString Ada")
assert_eq(super_method_key_string_label, "SuperMethodMethodKeyString Eve")
assert_eq(method_index_string_label, "MethodCallIndexString Neo")
assert_eq(method_index_value_label, "MethodCallIndexValue string Neo 3")
assert_eq(method_index_bool_label, "MethodCallIndexBool true")
assert_eq(method_index_object_label, "bot!")
assert_eq(method_index_object_field_label, "bot?")
assert_eq(method_index_object_index_label, "bot.")
assert_eq(type(make_mixed_profile_ast()["score"]), "number")
assert_eq(length(make_mixed_profile_ast()["name"]), 2)
assert_eq(to_str(make_mixed_profile_ast()["active"]), "true")
assert_eq(to_bool(make_mixed_profile_ast()["active"]), true)
assert_eq(direct_mixed_function_return_dict_label, "DirectMixedDictReturn number ok true nil")
expected_direct_mixed_profile_string is to_str({score: 32, name: "ok", active: true, missing: nil})
assert_eq(direct_mixed_function_return_dict_to_str_label, "DirectMixedDictString " + expected_direct_mixed_profile_string)
assert_eq(direct_mixed_function_return_dict_interp_label, "DirectMixedDictInterp " + expected_direct_mixed_profile_string)
assert_eq(direct_mixed_function_return_dict_concat_label, "DirectMixedDictConcat " + expected_direct_mixed_profile_string)
assert_eq(type(make_mixed_profile_ast().score), "number")
assert_eq(length(make_mixed_profile_ast().name), 2)
assert_eq(to_str(make_mixed_profile_ast().active), "true")
assert_eq(to_bool(make_mixed_profile_ast().active), true)
assert_eq(direct_mixed_function_return_dot_label, "DirectMixedDotReturn number ok true nil")
assert_eq(value_runtime_dict_index_label, "ValueRuntimeKey string Ada nil nil")
assert_eq(value_runtime_dot_label, "ValueRuntimeDot string Ada nil nil")
assert_eq(value_runtime_access_interp_label, "ValueRuntimeAccessInterp one Ada nil")
assert_eq(value_runtime_access_call_label, "ValueRuntimeAccessCall 3 true 3 true false")
assert_eq(value_runtime_access_eq_label, "ValueRuntimeAccessEq true true true false")
assert_eq(value_runtime_access_numeric_label, "ValueRuntimeNumeric 20 true 32 1")
assert_eq(value_runtime_access_numeric_ternary_label, "ValueRuntimeNumericTernary 17 18")
assert_eq(value_runtime_access_numeric_coalesce_label, "ValueRuntimeNumericCoalesce 6 18 10 19")
assert_eq(value_runtime_coalesce_receiver_label, "ValueRuntimeCoalesceReceiver string Ada number 16 string Fallback")
assert_eq(value_runtime_nested_access_label, "ValueRuntimeNested Ada 16 nil nil")
assert_eq(value_runtime_same_dict_shape_label, "ValueRuntimeSameDictShape number 16 nil nil number 16")
assert_eq(value_runtime_same_array_shape_label, "ValueRuntimeSameArrayShape number 16 nil nil bool true")
assert_eq(value_runtime_coalesce_shape_label, "ValueRuntimeCoalesceShape number 16 nil nil nil nil number 16")
assert_eq(value_runtime_string_method_label, "ValueRuntimeStringMethod ok ADA true 5")
assert_eq(value_runtime_string_chain_label, "ValueRuntimeStringChain SURA WASM wasm true -1")
assert_eq(value_runtime_string_call_label, "ValueRuntimeStringCall OK sura ada SURA wasm true 5 wasm")
assert_eq(value_runtime_string_search_alias_variant_label, "ValueRuntimeStringSearchAliasVariant 5 true true 5 true true")
assert_eq(value_runtime_len_method_label, "ValueRuntimeLenMethod 3 3 0 3")
assert_eq(value_runtime_alias_label, "ValueRuntimeAlias string one true 3 true one! nil nil string Ada 20")
assert_eq(value_runtime_alias_method_label, "ValueRuntimeAliasMethod ONE true one ADA 3 true")
assert_eq(value_runtime_collection_alias_label, "ValueRuntimeCollectionAlias array 2 one nil Ada nil 20 ONE true true 1")
assert_eq(value_runtime_collection_len_method_label, "ValueRuntimeCollectionLenMethod 2 2 3 3")
assert_eq(value_runtime_number_collection_alias_label, "ValueRuntimeNumberCollectionAlias 44 14 7 21")
assert_eq(value_runtime_mutable_label, "ValueRuntimeMutable two Grace 20")
assert_eq(value_runtime_array_module_alias_label, "ValueRuntimeArrayModuleAlias 3 44 14 7 21 true 1")
assert_eq(value_runtime_array_direct_alias_label, "ValueRuntimeArrayDirectAlias 3 44 14 7 21 true 1")
assert_eq(value_runtime_array_alias_variant_label, "ValueRuntimeArrayAliasVariant 3 14 1 1 true 3 14 1 true")
assert_eq(value_runtime_return_alias_label, "ValueRuntimeReturnAlias array 2 one Ada nil 20")
assert_eq(value_runtime_return_mutable_label, "ValueRuntimeReturnMutable TWO Lin 24")
assert_eq(value_runtime_method_mutable_label, "ValueRuntimeMethodMutable DOS Kim 28")
assert_eq(value_runtime_super_mutable_label, "ValueRuntimeSuperMutable TRES Seo 32")
assert_eq(if_runtime_label, "IfRuntime then array 2 true function true <Func fib>")
assert_eq(if_else_runtime_label, "IfRuntime else array 2 true function true <Func fib>")
assert_eq(match_runtime_label, "MatchRuntime bool array 2 true function true <Func fib>")
assert_eq(match_nil_runtime_label, "MatchRuntime nil array 2 true function true <Func fib>")
assert_eq(repeat_conflict_after, 1)
assert_eq(repeat_conflict_field_after, 1)
assert_eq(while_conflict_after, 1)
assert_eq(while_conflict_field_after, 1)
assert_eq(while_runtime_label, "WhileRuntime 2 array 2 true function true <Func fib>")
assert_eq(for_conflict_after, 1)
assert_eq(for_conflict_field_after, 1)
assert_eq(for_runtime_label, "ForRuntime 2 array 2 true function true <Func fib>")
assert_eq(foreach_array_conflict_after, 1)
assert_eq(foreach_array_conflict_field_after, 1)
assert_eq(foreach_dict_conflict_after, 1)
assert_eq(foreach_dict_conflict_field_after, 1)
assert_eq(type(mixed_profile[mixed_profile_active_lookup]), "bool")
assert_eq(to_bool(mixed_profile[mixed_profile_active_lookup]), true)
assert_eq(length(mixed_profile[mixed_profile_name_lookup]), 3)
assert_eq(to_str(mixed_profile[mixed_profile_score_lookup]), "16")
assert_eq(to_str(mixed_profile[mixed_profile_missing_lookup]), "nil")
assert_eq(mixed_dict_index_runtime_label, "DictRuntime bool true 3 16 nil")
assert_eq(not mixed_values[mixed_bool_idx], false)
assert_eq(not mixed_profile[mixed_profile_active_lookup], false)
assert_eq(not mixed_profile[mixed_profile_missing_lookup], true)
assert_eq(mixed_exact_not_label, "NotExact false false true")
assert_eq(mixed_values[mixed_bool_idx] and true, true)
assert_eq(mixed_profile[mixed_profile_active_lookup] and true, true)
assert_eq(mixed_profile[mixed_profile_missing_lookup] or true, true)
assert_eq(mixed_profile[mixed_profile_missing_lookup] and true, false)
assert_eq(mixed_exact_logic_label, "LogicExact true true true false")
assert_eq(mixed_values[mixed_bool_idx] == true, true)
assert_eq(mixed_profile[mixed_profile_active_lookup] == true, true)
assert_eq(mixed_profile[mixed_profile_missing_lookup] == nil, true)
assert_eq(mixed_profile[mixed_profile_score_lookup] != 17, true)
assert_eq(mixed_exact_eq_label, "EqExact true true true true")
assert_eq(mixed_values[mixed_score_idx] >= 16, true)
assert_eq(mixed_values[mixed_score_idx] < 20, true)
assert_eq(mixed_profile[mixed_profile_score_lookup] > 10, true)
assert_eq(mixed_profile[mixed_profile_score_lookup] <= 16, true)
assert_eq(mixed_exact_compare_label, "CompareExact true true true true")
assert_eq(mixed_values[mixed_score_idx] + 4, 20)
assert_eq(mixed_profile[mixed_profile_score_lookup] - 6, 10)
assert_eq(mixed_values[mixed_score_idx] * 2, 32)
assert_eq(mixed_profile[mixed_profile_score_lookup] / 4, 4)
assert_eq(mixed_profile[mixed_profile_score_lookup] % 5, 1)
assert_eq(mixed_exact_arith_label, "ArithExact 20 10 32 4 1")
assert_eq(mixed_values[mixed_score_idx] & 7, 0)
assert_eq(mixed_profile[mixed_profile_score_lookup] | 3, 19)
assert_eq(mixed_values[mixed_score_idx] ^ 3, 19)
assert_eq(mixed_profile[mixed_profile_score_lookup] << 1, 32)
assert_eq(mixed_values[mixed_score_idx] >> 1, 8)
assert_eq(~mixed_values[mixed_score_idx], -17)
assert_eq(mixed_exact_bitwise_label, "BitExact 0 19 19 32 8 -17")
expected_mixed_profile_string is to_str({score: 16, name: "Ada", active: true, missing: nil})
expected_nested_array_string is to_str([["Ada", "Grace"], {score: 16, name: "Ada", active: true, missing: nil}])
expected_nested_dict_string is to_str({names: ["Ada", "Grace"], profile: {score: 16, name: "Ada", active: true, missing: nil}})
assert_eq(to_str(mixed_profile), expected_mixed_profile_string)
assert_eq(dict_var_to_str_label, "DictVar " + expected_mixed_profile_string)
assert_eq(to_str([names, mixed_profile]), expected_nested_array_string)
assert_eq(to_str({names: names, profile: mixed_profile}), expected_nested_dict_string)
assert_eq(nested_collection_to_str_label, "NestedCollection " + expected_nested_array_string + " " + expected_nested_dict_string)
assert_eq(to_str(true ? "yes" : nil), "yes")
assert_eq(to_str(false ? "yes" : nil), "nil")
assert_eq(to_str(true ? [score, profile.name] : {name: profile.name}), "[16, \"Ada\"]")
assert_eq(to_str(false ? [score, profile.name] : {name: profile.name}), expected_name_dict_string)
assert_eq(to_str_mixed_ternary_label, "MixedStr yes nil [16, \"Ada\"] " + expected_name_dict_string)
assert_eq(type(true ? [1, 2] : {name: profile.name}), "array")
assert_eq(type(false ? [1, 2] : {name: profile.name}), "dict")
assert_eq(type(true ? "yes" : nil), "string")
assert_eq(type(false ? "yes" : nil), "nil")
assert_eq(type_mixed_ternary_label, "MixedType array dict string nil")
assert_eq(to_bool(profile), true)
assert_eq(to_bool(empty_profile), false)
assert_eq(to_bool(flags.missing), false)
assert_eq(bool_label, "Bools true false false")
names[1] is "Byron"
states[1] is true
states[2] is nil
assert_eq(names[1], "Byron")
assert_eq(names[1] + " Ada", "Byron Ada")
assert_eq(states[1], true)
assert_eq(states[2], nil)
assert_eq("Updated: " + names[1] + " " + states[1] + " " + states[2], "Updated: Byron true nil")
mutable_names is ["Ada"]
mutable_profile is {tag: "old"}
mutable_names[0] is 42
mutable_profile.tag is 7
compound_numbers is [2, 3]
compound_numbers[1] += 4
mutable_profile.tag += 5
mixed_num_dict[mixed_num_key_lookup] += 2
compound_label is "Compound {compound_numbers[1]} {mutable_profile.tag} {mixed_num_dict[mixed_num_key_lookup]}"
compound_names is ["Ada"]
compound_profile is {name: "Ada"}
compound_key is "name"
compound_names[0] += "!"
compound_profile.name += "?"
compound_profile[compound_key] += "#"
compound_string_label is "CompoundStr {compound_names[0]} {compound_profile.name} {compound_profile[compound_key]}"
assert_eq(mutable_names[0], 42)
assert_eq(mutable_profile.tag, 12)
assert_eq("Mutable: " + mutable_names[0] + " " + mutable_profile.tag, "Mutable: 42 12")
assert_eq(compound_numbers[1], 7)
assert_eq(mutable_profile.tag, 12)
assert_eq(mixed_num_dict[mixed_num_key_lookup], 18)
assert_eq(compound_label, "Compound 7 12 18")
assert_eq(compound_names[0], "Ada!")
assert_eq(compound_profile.name, "Ada?#")
assert_eq(compound_profile[compound_key], "Ada?#")
assert_eq(compound_string_label, "CompoundStr Ada! Ada?# Ada?#")
field_values is {text: "", active: false, missing: nil, items: [], meta: {}, handler: zero_a_ast}
field_values.text is "field"
field_values.active is true
field_values.missing is nil
field_values.items is [1, "x"]
field_values.meta is {ok: true}
field_values.handler is 3
field_assign_label is "FieldAssign {type(field_values.text)} {to_str(field_values.text)} {type(field_values.active)} {to_bool(field_values.active)} {type(field_values.missing)} {to_bool(field_values.missing)} {type(field_values.items)} {length(field_values.items)} {type(field_values.meta)} {to_bool(field_values.meta)} {type(field_values.handler)} {to_bool(field_values.handler)} {to_str(field_values.handler)}"
assert_eq(field_assign_label, "FieldAssign string field bool true nil false array 2 dict true number true 3")
meta.score is 10
meta["bonus"] is 4
assert_eq(meta.score, 10)
assert_eq(meta["bonus"], 4)
try_flag is 0
try
  try_flag is 2
catch err
  try_flag is 9
finally do
  try_flag += 3
end
assert_eq(try_flag, 5)
try_throw_flag is 0
try
  try_throw_flag is 1
  throw 99
  try_throw_flag is 7
catch err
  try_throw_flag += 10
finally do
  try_throw_flag += 100
end
assert_eq(try_throw_flag, 111)
try_payload_flag is 0
try
  throw 42
catch err
  try_payload_flag is err + 1
finally do
  try_payload_flag += 100
end
assert_eq(try_payload_flag, 143)
try_string_payload is ""
try
  throw "boom"
catch err
  try_string_payload is err + "!"
end
assert_eq(try_string_payload, "boom!")
try_function_payload is ""
try
  caught_from_function is throw_text_ast()
catch err
  try_function_payload is err + "!"
end
assert_eq(try_function_payload, "fn boom!")
try_function_number_payload is 0
try
  caught_number_from_function is throw_number_ast()
catch err
  try_function_number_payload is err + 5
end
assert_eq(try_function_number_payload, 17)
try_function_value_payload is ""
try
  caught_function_value is throw_function_ast()
catch err
  try_function_value_payload is "Function throw {type(err)} {to_bool(err)}"
end
assert_eq(try_function_value_payload, "Function throw function true")
try_function_value_call is 0
try
  caught_function_value_call is throw_function_ast()
catch err
  try_function_value_call is err(18)
end
assert_eq(try_function_value_call, 36)
try_function_param_payload_call is 0
try
  caught_function_param_payload is throw_selected_function_ast(block_double_ast)
catch err
  try_function_param_payload_call is err(21)
end
assert_eq(try_function_param_payload_call, 42)
try_method_payload is ""
try
  caught_from_method is method_thrower.throw_text()
catch err
  try_method_payload is err + "!"
end
assert_eq(try_method_payload, "method boom!")
try_method_function_value_payload is ""
try
  caught_method_function_value is method_thrower.throw_function_value()
catch err
  try_method_function_value_payload is "Method function throw {type(err)} {to_bool(err)}"
end
assert_eq(try_method_function_value_payload, "Method function throw function true")
try_method_function_value_call is 0
try
  caught_method_function_value_call is method_thrower.throw_function_value()
catch err
  try_method_function_value_call is err(19)
end
assert_eq(try_method_function_value_call, 38)
try_super_method_payload is super_method_thrower.catch_super_text()
assert_eq(try_super_method_payload, "method boom via super")
try_super_method_function_value_payload is super_method_thrower.catch_super_function_value()
assert_eq(try_super_method_function_value_payload, "Super function throw function true 40")
try_super_echo_label is super_method_thrower.super_echo_label(super_echo_source)
assert_eq(try_super_echo_label, "Super echo string local-super")
try_bool_payload is false
try
  throw true
catch err
  try_bool_payload is err
end
assert_eq(try_bool_payload, true)
try_nil_payload is ""
try
  throw nil
catch err
  try_nil_payload is "Nil " + err
end
assert_eq(try_nil_payload, "Nil nil")
try_array_payload_flag is 0
try
  throw [1, 2]
catch err
  if err then
    try_array_payload_flag is 1
  end
end
assert_eq(try_array_payload_flag, 1)
try_dict_payload_flag is 0
try
  throw {ready: 1}
catch err
  if err then
    try_dict_payload_flag is 1
  end
end
assert_eq(try_dict_payload_flag, 1)
try_mixed_payload_after is 0
try
  if false then
    throw "boom"
  else
    throw 7
  end
catch err
  try_mixed_payload_after is err + 1
end
assert_eq(try_mixed_payload_after, 8)
nested_try_payload is 0
try
  try
    throw 5
  catch inner
    nested_try_payload += 10
    throw inner
  end
  nested_try_payload is 99
catch err
  nested_try_payload is nested_try_payload + err
end
assert_eq(nested_try_payload, 15)
assert_eq(label.len(), 8)
assert_eq(label[6], "1")
assert_eq(label[7], "6")
assert_eq(score_concat_label.len(), 9)
assert_eq(score_concat_label[6], "1")
assert_eq(score_concat_label[7], "6")
assert_eq(score_concat_label[8], "!")
assert_eq(mutable_label, "Go!16")
assert_eq(score_suffix, "16!")
assert_eq(compound_op_score, 9)
assert_eq(compound_ops_label, "CompoundOps 9")
assert_eq(name_join, "AdaGrace")
assert_eq(indexed_name_join, "AdaGrace")
assert_eq(name_index_sum, 1)
assert_eq(inline_name_join, "SR")
assert_eq(profile_key_join, "namecity")
assert_eq(profile_value_join, "AdaSeoul")
assert_eq(meta_key_join, "scorebonus")
assert_eq(meta_value_sum, 12)
assert_eq(dict_single_count, 0)
assert_eq(literal_braces.len(), 12)
loop_score is sum_to_ast(4) + imported_ast(3)
console.log("ast console", loop_score)
console.info("ast console", true, nil)
console.warn("ast console warn")
console_error("ast direct console", loop_score)
if score == 16 then
  print loop_score
else
  print 0
end
'@ | Set-Content -LiteralPath $astSource -Encoding UTF8
    & $enginePath --ast-json --out $astJson $astSource | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "AST JSON export for WASM target smoke failed with exit code $LASTEXITCODE"
    }
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $astJson -Out $astWat -WasmOut $astWasm -AstJson -Engine $enginePath | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $astWasm)) {
        throw "AST JSON WASM target WAT validation/binary emission failed with exit code $LASTEXITCODE"
    }
    $astWatText = [System.IO.File]::ReadAllText($astWat, [System.Text.Encoding]::ASCII)
    if ($astWatText -notmatch "AST JSON input: sura\.ast\.v1" -or
        $astWatText -notmatch '\(func \$add_ast \(export "add_ast"\)' -or
        $astWatText -notmatch '\(func \$label_pair \(export "label_pair"\)' -or
        $astWatText -notmatch '\(func \$nested_label_pair \(export "nested_label_pair"\)' -or
        $astWatText -notmatch '\(func \$nested_method_label \(export "nested_method_label"\)' -or
        $astWatText -notmatch '\(func \$if_local_method_label \(export "if_local_method_label"\)' -or
        $astWatText -notmatch '\(func \$match_local_method_label \(export "match_local_method_label"\)' -or
        $astWatText -notmatch '\(func \$bool_state_line \(export "bool_state_line"\)' -or
        $astWatText -notmatch '\(func \$bool_gate_ast \(export "bool_gate_ast"\)' -or
        $astWatText -notmatch '\(func \$throw_text_ast \(export "throw_text_ast"\)' -or
        $astWatText -notmatch '\(func \$throw_number_ast \(export "throw_number_ast"\)' -or
        $astWatText -notmatch '\(func \$describe_function_ast \(export "describe_function_ast"\)' -or
        $astWatText -notmatch '\(func \$pass_function_ast \(export "pass_function_ast"\)' -or
        $astWatText -notmatch '\(func \$call_function_param_ast \(export "call_function_param_ast"\)' -or
        $astWatText -notmatch '\(func \$describe_call_function_param_ast \(export "describe_call_function_param_ast"\)' -or
        $astWatText -notmatch '\(func \$call_dynamic_function_param_ast \(export "call_dynamic_function_param_ast"\)' -or
        $astWatText -notmatch '\(func \$call_dynamic_local_function_ast \(export "call_dynamic_local_function_ast"\)' -or
        $astWatText -notmatch '\(func \$triple_ast \(export "triple_ast"\)' -or
        $astWatText -notmatch '\(func \$pick_function_ast \(export "pick_function_ast"\)' -or
        $astWatText -notmatch '\(func \$throw_function_ast \(export "throw_function_ast"\)' -or
        $astWatText -notmatch '\(func \$throw_selected_function_ast \(export "throw_selected_function_ast"\)' -or
        $astWatText -notmatch '\(func \$conflict_probe \(export "conflict_probe"\)' -or
        $astWatText -notmatch '\(func \$pick_label_ast \(export "pick_label_ast"\)' -or
        $astWatText -notmatch '\(func \$pick_bool_ast \(export "pick_bool_ast"\)' -or
        $astWatText -notmatch '\(func \$make_values_ast \(export "make_values_ast"\)' -or
        $astWatText -notmatch '\(func \$make_profile_ast \(export "make_profile_ast"\)' -or
        $astWatText -notmatch '\(func \$pass_values_ast \(export "pass_values_ast"\)' -or
        $astWatText -notmatch '\(func \$pass_profile_ast \(export "pass_profile_ast"\)' -or
        $astWatText -notmatch '\(func \$choose_values_ast \(export "choose_values_ast"\)' -or
        $astWatText -notmatch '\(func \$choose_profile_ast \(export "choose_profile_ast"\)' -or
        $astWatText -notmatch '\(func \$function_alias_values_text_ast \(export "function_alias_values_text_ast"\)' -or
        $astWatText -notmatch '\(func \$function_alias_empty_values_text_ast \(export "function_alias_empty_values_text_ast"\)' -or
        $astWatText -notmatch '\(func \$function_alias_profile_text_ast \(export "function_alias_profile_text_ast"\)' -or
        $astWatText -notmatch '\(func \$function_alias_empty_profile_text_ast \(export "function_alias_empty_profile_text_ast"\)' -or
        $astWatText -notmatch '\(func \$function_alias_profile_bonus_text_ast \(export "function_alias_profile_bonus_text_ast"\)' -or
        $astWatText -notmatch '\(func \$inline_function_expr_values_ast \(export "inline_function_expr_values_ast"\)' -or
        $astWatText -notmatch '\(func \$direct_inline_function_expr_values_ast \(export "direct_inline_function_expr_values_ast"\)' -or
        $astWatText -notmatch '\(func \$__sura_func_expr_local_inner_ast_\d+ \(export "__sura_func_expr_local_inner_ast_\d+"\)' -or
        $astWatText -notmatch '\(func \$__sura_func_expr__lambda__\d+ \(export "__sura_func_expr__lambda__\d+"\)' -or
        $astWatText -notmatch 'call \$__sura_func_expr_local_inner_ast_\d+' -or
        $astWatText -notmatch '\(func \$choose_values_local_ast \(export "choose_values_local_ast"\)' -or
        $astWatText -notmatch '\(func \$choose_profile_local_ast \(export "choose_profile_local_ast"\)' -or
        $astWatText -notmatch '\(func \$choose_tagged_if_ast \(export "choose_tagged_if_ast"\)' -or
        $astWatText -notmatch '\(func \$local_tagged_from_if_len_ast \(export "local_tagged_from_if_len_ast"\)' -or
        $astWatText -notmatch '\(func \$choose_tagged_match_ast \(export "choose_tagged_match_ast"\)' -or
        $astWatText -notmatch '\(func \$local_tagged_from_match_len_ast \(export "local_tagged_from_match_len_ast"\)' -or
        $astWatText -notmatch '\(func \$sum_to_ast \(export "sum_to_ast"\)' -or
        $astWatText -notmatch '\(func \$imported_ast \(export "imported_ast"\)' -or
        $astWatText -notmatch '\(func \$block_double_ast \(export "block_double_ast"\)' -or
        $astWatText -notmatch '\(func \$format_pair_ast \(export "format_pair_ast"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstPoint \(export "__sura_new_AstPoint"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstBase \(export "__sura_new_AstBase"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstChild \(export "__sura_new_AstChild"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstFunctionHolder \(export "__sura_new_AstFunctionHolder"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstFunctionChild \(export "__sura_new_AstFunctionChild"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstMethodThrower \(export "__sura_new_AstMethodThrower"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstSuperMethodThrower \(export "__sura_new_AstSuperMethodThrower"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstTagged \(export "__sura_new_AstTagged"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstParamTagged \(export "__sura_new_AstParamTagged"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstTaggedBox \(export "__sura_new_AstTaggedBox"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstTaggedBoxChild \(export "__sura_new_AstTaggedBoxChild"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstCollectionTagged \(export "__sura_new_AstCollectionTagged"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstCollectionParent \(export "__sura_new_AstCollectionParent"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstCollectionChild \(export "__sura_new_AstCollectionChild"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstFieldConflict \(export "__sura_new_AstFieldConflict"\)' -or
        $astWatText -notmatch '\(func \$__sura_new_AstRecursive \(export "__sura_new_AstRecursive"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstPoint_sum \(export "__sura_method_AstPoint_sum"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstPoint_shifted \(export "__sura_method_AstPoint_shifted"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstPoint_shifted_conflict \(export "__sura_method_AstPoint_shifted_conflict"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstBase_root_value \(export "__sura_method_AstBase_root_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstBase_root_plus \(export "__sura_method_AstBase_root_plus"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstBase_root_label \(export "__sura_method_AstBase_root_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstChild_root_plus_one \(export "__sura_method_AstChild_root_plus_one"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstChild_root_plus_delta \(export "__sura_method_AstChild_root_plus_delta"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstChild_root_label_child \(export "__sura_method_AstChild_root_label_child"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_handler_type \(export "__sura_method_AstFunctionHolder_handler_type"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_handler_truth \(export "__sura_method_AstFunctionHolder_handler_truth"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_handler_value \(export "__sura_method_AstFunctionHolder_handler_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_choose_handler \(export "__sura_method_AstFunctionHolder_choose_handler"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_call_handler \(export "__sura_method_AstFunctionHolder_call_handler"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_handler_label \(export "__sura_method_AstFunctionHolder_handler_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionHolder_same_handler \(export "__sura_method_AstFunctionHolder_same_handler"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionChild_inherited_handler \(export "__sura_method_AstFunctionChild_inherited_handler"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFunctionChild_inherited_label \(export "__sura_method_AstFunctionChild_inherited_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstMethodThrower_throw_text \(export "__sura_method_AstMethodThrower_throw_text"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstMethodThrower_throw_function_value \(export "__sura_method_AstMethodThrower_throw_function_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstMethodThrower_echo_arg \(export "__sura_method_AstMethodThrower_echo_arg"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstSuperMethodThrower_catch_super_text \(export "__sura_method_AstSuperMethodThrower_catch_super_text"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstSuperMethodThrower_catch_super_function_value \(export "__sura_method_AstSuperMethodThrower_catch_super_function_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstSuperMethodThrower_super_echo_label \(export "__sura_method_AstSuperMethodThrower_super_echo_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTagged_kind_text \(export "__sura_method_AstTagged_kind_text"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTagged_is_active \(export "__sura_method_AstTagged_is_active"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTagged_none_value \(export "__sura_method_AstTagged_none_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTagged_active_gate \(export "__sura_method_AstTagged_active_gate"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTagged_peer \(export "__sura_method_AstTagged_peer"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTagged_peer_kind_len \(export "__sura_method_AstTagged_peer_kind_len"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTaggedBox_peer_kind_label \(export "__sura_method_AstTaggedBox_peer_kind_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTaggedBox_peer_kind_field_label \(export "__sura_method_AstTaggedBox_peer_kind_field_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTaggedBox_peer_kind_index_label \(export "__sura_method_AstTaggedBox_peer_kind_index_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTaggedBoxChild_child_peer_kind_label \(export "__sura_method_AstTaggedBoxChild_child_peer_kind_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTaggedBoxChild_child_peer_kind_field_label \(export "__sura_method_AstTaggedBoxChild_child_peer_kind_field_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstTaggedBoxChild_child_peer_kind_index_label \(export "__sura_method_AstTaggedBoxChild_child_peer_kind_index_label"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_kind_text \(export "__sura_method_AstParamTagged_kind_text"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_is_active \(export "__sura_method_AstParamTagged_is_active"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_none_value \(export "__sura_method_AstParamTagged_none_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_active_gate \(export "__sura_method_AstParamTagged_active_gate"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_kind_badge \(export "__sura_method_AstParamTagged_kind_badge"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_state_line \(export "__sura_method_AstParamTagged_state_line"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_kind_suffix \(export "__sura_method_AstParamTagged_kind_suffix"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstParamTagged_state_prefix \(export "__sura_method_AstParamTagged_state_prefix"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_full_items_value \(export "__sura_method_AstCollectionTagged_full_items_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_empty_items_value \(export "__sura_method_AstCollectionTagged_empty_items_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_full_profile_value \(export "__sura_method_AstCollectionTagged_full_profile_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_empty_profile_value \(export "__sura_method_AstCollectionTagged_empty_profile_value"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_pick_items \(export "__sura_method_AstCollectionTagged_pick_items"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_pick_profile \(export "__sura_method_AstCollectionTagged_pick_profile"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_pick_items_local \(export "__sura_method_AstCollectionTagged_pick_items_local"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_pick_profile_local \(export "__sura_method_AstCollectionTagged_pick_profile_local"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_pick_items_local_gate \(export "__sura_method_AstCollectionTagged_pick_items_local_gate"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_pick_profile_local_gate \(export "__sura_method_AstCollectionTagged_pick_profile_local_gate"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionTagged_ctor_param_summary \(export "__sura_method_AstCollectionTagged_ctor_param_summary"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionParent_super_ctor_summary \(export "__sura_method_AstCollectionParent_super_ctor_summary"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionParent_describe_parent \(export "__sura_method_AstCollectionParent_describe_parent"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionParent_values_parent \(export "__sura_method_AstCollectionParent_values_parent"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionParent_profile_parent \(export "__sura_method_AstCollectionParent_profile_parent"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionChild_describe_via_super \(export "__sura_method_AstCollectionChild_describe_via_super"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionChild_values_via_super \(export "__sura_method_AstCollectionChild_values_via_super"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstCollectionChild_profile_via_super \(export "__sura_method_AstCollectionChild_profile_via_super"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstFieldConflict_bump \(export "__sura_method_AstFieldConflict_bump"\)' -or
        $astWatText -notmatch '\(func \$__sura_method_AstRecursive_countdown \(export "__sura_method_AstRecursive_countdown"\)' -or
        $astWatText -notmatch 'top-level numeric function-expression promotion' -or
        $astWatText -notmatch 'partial class lowering: AstPoint' -or
        $astWatText -notmatch 'call \$add_ast' -or
        $astWatText -notmatch 'call \$label_pair' -or
        $astWatText -notmatch 'call \$nested_label_pair' -or
        $astWatText -notmatch 'call \$nested_method_label' -or
        $astWatText -notmatch 'call \$if_local_method_label' -or
        $astWatText -notmatch 'call \$match_local_method_label' -or
        $astWatText -notmatch 'call \$bool_state_line' -or
        $astWatText -notmatch 'call \$bool_gate_ast' -or
        $astWatText -notmatch 'call \$conflict_probe' -or
        $astWatText -notmatch 'call \$pick_label_ast' -or
        $astWatText -notmatch 'call \$pick_bool_ast' -or
        $astWatText -notmatch 'call \$make_values_ast' -or
        $astWatText -notmatch 'call \$make_profile_ast' -or
        $astWatText -notmatch 'call \$pass_values_ast' -or
        $astWatText -notmatch 'call \$pass_profile_ast' -or
        $astWatText -notmatch 'call \$choose_values_ast' -or
        $astWatText -notmatch 'call \$choose_profile_ast' -or
        $astWatText -notmatch 'call \$choose_values_local_ast' -or
        $astWatText -notmatch 'call \$choose_profile_local_ast' -or
        $astWatText -notmatch 'call \$choose_values_local_gate_ast' -or
        $astWatText -notmatch 'call \$choose_profile_local_gate_ast' -or
        $astWatText -notmatch 'call \$local_tagged_kind_len_ast' -or
        $astWatText -notmatch 'call \$local_tagged_field_len_ast' -or
        $astWatText -notmatch 'call \$local_tagged_gate_ast' -or
        $astWatText -notmatch 'call \$make_tagged_local_ast' -or
        $astWatText -notmatch 'call \$choose_tagged_local_ast' -or
        $astWatText -notmatch 'call \$local_tagged_from_call_len_ast' -or
        $astWatText -notmatch 'call \$choose_tagged_if_ast' -or
        $astWatText -notmatch 'call \$local_tagged_from_if_len_ast' -or
        $astWatText -notmatch 'call \$choose_tagged_match_ast' -or
        $astWatText -notmatch 'call \$local_tagged_from_match_len_ast' -or
        $astWatText -notmatch 'call \$local_tagged_from_method_len_ast' -or
        $astWatText -notmatch 'call \$tagged_param_kind_len_ast' -or
        $astWatText -notmatch 'call \$tagged_param_peer_len_ast' -or
        $astWatText -notmatch 'call \$tagged_param_field_label_ast' -or
        $astWatText -notmatch 'call \$tagged_param_index_label_ast' -or
        $astWatText -notmatch 'call \$direct_tagged_call_len_ast' -or
        $astWatText -notmatch 'call \$direct_tagged_ternary_len_ast' -or
        $astWatText -notmatch 'call \$direct_tagged_receiver_arg_len_ast' -or
        $astWatText -notmatch 'call \$direct_tagged_index_label_ast' -or
        $astWatText -notmatch 'call \$direct_tagged_ternary_index_label_ast' -or
        $astWatText -notmatch 'call \$sum_to_ast' -or
        $astWatText -notmatch 'call \$imported_ast' -or
        $astWatText -notmatch 'call \$block_double_ast' -or
        $astWatText -notmatch 'call \$format_pair_ast' -or
        $astWatText -notmatch 'call \$__sura_new_AstPoint' -or
        $astWatText -notmatch 'call \$__sura_new_AstBase' -or
        $astWatText -notmatch 'call \$__sura_new_AstChild' -or
        $astWatText -notmatch 'call \$__sura_new_AstFunctionHolder' -or
        $astWatText -notmatch 'call \$__sura_new_AstFunctionChild' -or
        $astWatText -notmatch 'call \$__sura_new_AstMethodThrower' -or
        $astWatText -notmatch 'call \$__sura_new_AstSuperMethodThrower' -or
        $astWatText -notmatch 'call \$__sura_new_AstTagged' -or
        $astWatText -notmatch 'call \$__sura_new_AstParamTagged' -or
        $astWatText -notmatch 'call \$__sura_new_AstCollectionTagged' -or
        $astWatText -notmatch 'call \$__sura_new_AstCollectionChild' -or
        $astWatText -notmatch 'call \$__sura_new_AstFieldConflict' -or
        $astWatText -notmatch 'call \$__sura_new_AstRecursive' -or
        $astWatText -notmatch 'call \$__sura_method_AstPoint_sum' -or
        $astWatText -notmatch 'call \$__sura_method_AstPoint_shifted' -or
        $astWatText -notmatch 'call \$__sura_method_AstPoint_shifted_conflict' -or
        $astWatText -notmatch 'call \$__sura_method_AstBase_root_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstBase_root_plus' -or
        $astWatText -notmatch 'call \$__sura_method_AstBase_root_label' -or
        $astWatText -notmatch 'call \$__sura_method_AstChild_root_plus_one' -or
        $astWatText -notmatch 'call \$__sura_method_AstChild_root_plus_delta' -or
        $astWatText -notmatch 'call \$__sura_method_AstChild_root_label_child' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionHolder_handler_type' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionHolder_handler_truth' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionHolder_handler_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionHolder_choose_handler' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionHolder_handler_label' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionHolder_same_handler' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionChild_inherited_handler' -or
        $astWatText -notmatch 'call \$__sura_method_AstFunctionChild_inherited_label' -or
        $astWatText -notmatch 'call \$__sura_method_AstMethodThrower_throw_text' -or
        $astWatText -notmatch 'call \$__sura_method_AstMethodThrower_throw_function_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstMethodThrower_echo_arg' -or
        $astWatText -notmatch 'call \$__sura_method_AstSuperMethodThrower_catch_super_text' -or
        $astWatText -notmatch 'call \$__sura_method_AstSuperMethodThrower_catch_super_function_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstSuperMethodThrower_super_echo_label' -or
        $astWatText -notmatch 'call \$__sura_method_AstTagged_kind_text' -or
        $astWatText -notmatch 'call \$__sura_method_AstTagged_is_active' -or
        $astWatText -notmatch 'call \$__sura_method_AstTagged_none_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstTagged_active_gate' -or
        $astWatText -notmatch 'call \$__sura_method_AstTagged_peer' -or
        $astWatText -notmatch 'call \$__sura_method_AstTagged_peer_kind_len' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_kind_text' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_is_active' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_none_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_active_gate' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_kind_badge' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_state_line' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_kind_suffix' -or
        $astWatText -notmatch 'call \$__sura_method_AstParamTagged_state_prefix' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_full_items_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_empty_items_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_full_profile_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_empty_profile_value' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_pick_items' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_pick_profile' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_pick_items_local' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_pick_profile_local' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_pick_items_local_gate' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_pick_profile_local_gate' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionTagged_ctor_param_summary' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionParent_super_ctor_summary' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionParent_describe_parent' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionParent_values_parent' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionParent_profile_parent' -or
        $astWatText -notmatch 'call \$__sura_method_AstCollectionChild_describe_via_super' -or
        $astWatText -notmatch 'call \$__sura_method_AstFieldConflict_bump' -or
        $astWatText -notmatch 'call \$__sura_method_AstRecursive_countdown' -or
        $astWatText -notmatch 'local\.set \$point' -or
        $astWatText -notmatch 'local\.set \$child' -or
        $astWatText -notmatch 'local\.set \$tagged' -or
        $astWatText -notmatch 'local\.set \$param_tagged' -or
        $astWatText -notmatch 'local\.set \$collection_tagged' -or
        $astWatText -notmatch 'local\.set \$field_conflict_num' -or
        $astWatText -notmatch 'local\.set \$field_conflict_text' -or
        $astWatText -notmatch 'local\.set \$recursive_counter' -or
        $astWatText -notmatch 'local\.set \$function_holder' -or
        $astWatText -notmatch 'local\.set \$function_holder_alt' -or
        $astWatText -notmatch 'local\.set \$function_child' -or
        $astWatText -notmatch 'local\.set \$method_thrower' -or
        $astWatText -notmatch 'local\.set \$super_method_thrower' -or
        $astWatText -notmatch 'local\.set \$super_echo_source' -or
        $astWatText -notmatch 'local\.set \$meta' -or
        $astWatText -notmatch 'local\.set \$profile' -or
        $astWatText -notmatch 'local\.set \$flags' -or
        $astWatText -notmatch 'local\.set \$names' -or
        $astWatText -notmatch 'local\.set \$states' -or
        $astWatText -notmatch 'local\.set \$bools' -or
        $astWatText -notmatch 'local\.set \$dynamic_idx' -or
        $astWatText -notmatch 'local\.set \$mixed_to_str_idx' -or
        $astWatText -notmatch 'local\.set \$mixed_type_idx' -or
        $astWatText -notmatch 'local\.set \$mixed_bool_idx' -or
        $astWatText -notmatch 'local\.set \$mixed_len_idx' -or
        $astWatText -notmatch 'local\.set \$repeat_conflict_after' -or
        $astWatText -notmatch 'local\.set \$repeat_conflict_field_after' -or
        $astWatText -notmatch 'local\.set \$while_conflict_after' -or
        $astWatText -notmatch 'local\.set \$while_conflict_field_after' -or
        $astWatText -notmatch 'local\.set \$for_conflict_after' -or
        $astWatText -notmatch 'local\.set \$for_conflict_field_after' -or
        $astWatText -notmatch 'local\.set \$foreach_array_conflict_after' -or
        $astWatText -notmatch 'local\.set \$foreach_array_conflict_field_after' -or
        $astWatText -notmatch 'local\.set \$foreach_dict_conflict_after' -or
        $astWatText -notmatch 'local\.set \$foreach_dict_conflict_field_after' -or
        $astWatText -notmatch 'local\.set \$short_circuit_and_ok' -or
        $astWatText -notmatch 'local\.set \$short_circuit_or_ok' -or
        $astWatText -notmatch 'local\.set \$mixed_profile_name_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_profile_active_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_profile_missing_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_profile_score_lookup' -or
        $astWatText -notmatch 'local\.set \$profile_key_lookup' -or
        $astWatText -notmatch 'local\.set \$profile_update_key' -or
        $astWatText -notmatch 'local\.set \$empty_values' -or
        $astWatText -notmatch 'local\.set \$empty_profile' -or
        $astWatText -notmatch 'local\.set \$nested' -or
        $astWatText -notmatch 'local\.set \$nested_arrays' -or
        $astWatText -notmatch 'local\.set \$nested_profiles' -or
        $astWatText -notmatch 'local\.set \$mutable_names' -or
        $astWatText -notmatch 'local\.set \$mutable_profile' -or
        $astWatText -notmatch '\(local \$__sura_wasm_dict_tmp i32\)' -or
        $astWatText -notmatch 'call \$__sura_dict_put' -or
        $astWatText -notmatch 'i32\.const 12' -or
        $astWatText -notmatch 'call \$__sura_dict_get' -or
        $astWatText -notmatch 'call \$__sura_dict_set' -or
        $astWatText -notmatch '\(func \$__sura_string_hash' -or
        $astWatText -notmatch 'call \$__sura_string_hash' -or
        $astWatText -notmatch 'call \$__sura_value_eq' -or
        $astWatText -notmatch 'call \$__sura_value_is_truthy' -or
        $astWatText -notmatch 'call \$__sura_value_array' -or
        $astWatText -notmatch 'call \$__sura_value_dict' -or
        $astWatText -notmatch 'call \$__sura_value_dynamic_array' -or
        $astWatText -notmatch 'call \$__sura_value_dynamic_dict' -or
        $astWatText -notmatch 'call \$__sura_value_array_to_string' -or
        $astWatText -notmatch 'call \$__sura_value_dict_to_string' -or
        $astWatText -notmatch 'call \$__sura_value_function' -or
        $astWatText -notmatch '\(func \$__sura_string_eq' -or
        $astWatText -notmatch '\(global \$__sura_exception_thrown \(mut i32\) \(i32.const 0\)\)' -or
        $astWatText -notmatch '\(global \$__sura_exception_value \(mut i32\) \(i32.const 0\)\)' -or
        $astWatText -notmatch 'local\.set \$truth_score' -or
        $astWatText -notmatch 'local\.set \$ternary_label' -or
        $astWatText -notmatch 'local\.set \$ternary_bool' -or
        $astWatText -notmatch 'local\.set \$ternary_nil' -or
        $astWatText -notmatch 'structured try/catch/finally lowering uses hidden thrown flag and payload locals' -or
        $astWatText -notmatch 'local\.set \$try_flag' -or
        $astWatText -notmatch 'local\.set \$try_throw_flag' -or
        $astWatText -notmatch 'local\.set \$try_payload_flag' -or
        $astWatText -notmatch 'local\.set \$try_string_payload' -or
        $astWatText -notmatch 'local\.set \$try_function_payload' -or
        $astWatText -notmatch 'local\.set \$try_function_number_payload' -or
        $astWatText -notmatch 'local\.set \$try_function_value_payload' -or
        $astWatText -notmatch 'local\.set \$caught_function_value' -or
        $astWatText -notmatch 'local\.set \$try_function_value_call' -or
        $astWatText -notmatch 'local\.set \$caught_function_value_call' -or
        $astWatText -notmatch 'local\.set \$try_function_param_payload_call' -or
        $astWatText -notmatch 'local\.set \$caught_function_param_payload' -or
        $astWatText -notmatch 'local\.set \$try_method_payload' -or
        $astWatText -notmatch 'local\.set \$caught_from_method' -or
        $astWatText -notmatch 'local\.set \$try_method_function_value_payload' -or
        $astWatText -notmatch 'local\.set \$caught_method_function_value' -or
        $astWatText -notmatch 'local\.set \$try_method_function_value_call' -or
        $astWatText -notmatch 'local\.set \$caught_method_function_value_call' -or
        $astWatText -notmatch 'local\.set \$try_super_method_payload' -or
        $astWatText -notmatch 'local\.set \$try_super_method_function_value_payload' -or
        $astWatText -notmatch 'local\.set \$try_super_echo_label' -or
        $astWatText -notmatch 'local\.set \$try_bool_payload' -or
        $astWatText -notmatch 'local\.set \$try_nil_payload' -or
        $astWatText -notmatch 'local\.set \$try_array_payload_flag' -or
        $astWatText -notmatch 'local\.set \$try_dict_payload_flag' -or
        $astWatText -notmatch 'local\.set \$try_mixed_payload_after' -or
        $astWatText -notmatch 'local\.set \$nested_try_payload' -or
        $astWatText -notmatch 'local\.set \$__try_thrown' -or
        $astWatText -notmatch 'local\.set \$__try_value' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_string_concat.*?local\.set \$try_string_payload' -or
        $astWatText -notmatch '(?s)\(func \$throw_text_ast.*?global\.set \$__sura_exception_value\s+i32\.const 1\s+global\.set \$__sura_exception_thrown\s+i32\.const 0\s+return' -or
        $astWatText -notmatch '(?s)\(func \$throw_number_ast.*?global\.set \$__sura_exception_value\s+i32\.const 1\s+global\.set \$__sura_exception_thrown\s+i32\.const 0\s+return' -or
        $astWatText -notmatch '(?s)call \$throw_text_ast\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?global\.get \$__sura_exception_value_tagged\s+local\.set \$__try_value_tagged\d+.*?br \$__try_end\d+' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_string_concat.*?local\.set \$try_function_payload' -or
        $astWatText -notmatch '(?s)call \$throw_number_ast\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?br \$__try_end\d+' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err\s+i32\.const 5\s+i32\.add\s+local\.set \$try_function_number_payload' -or
        $astWatText -notmatch '(?s)\(func \$throw_function_ast.*?i32\.const \d+.*?call \$__sura_value_function.*?global\.set \$__sura_exception_value_tagged.*?call \$__sura_value_payload\s+global\.set \$__sura_exception_value\s+i32\.const 1\s+global\.set \$__sura_exception_thrown\s+i32\.const 0\s+return' -or
        $astWatText -notmatch '(?s)call \$throw_function_ast\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?br \$__try_end\d+' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.get \$err.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy.*?local\.set \$try_function_value_payload' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstMethodThrower_throw_text.*?global\.set \$__sura_exception_value\s+i32\.const 1\s+global\.set \$__sura_exception_thrown\s+i32\.const 0\s+return' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstMethodThrower_throw_text\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?br \$__try_end\d+' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_string_concat.*?local\.set \$try_method_payload' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstMethodThrower_throw_function_value.*?i32\.const \d+.*?call \$__sura_value_function.*?global\.set \$__sura_exception_value_tagged.*?call \$__sura_value_payload\s+global\.set \$__sura_exception_value\s+i32\.const 1\s+global\.set \$__sura_exception_thrown\s+i32\.const 0\s+return' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstMethodThrower_throw_function_value\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?br \$__try_end\d+' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.get \$err.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy.*?local\.set \$try_method_function_value_payload' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstSuperMethodThrower_catch_super_text.*?call \$__sura_method_AstMethodThrower_throw_text\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?br \$__try_end\d+.*?local\.get \$__try_value\d+\s+local\.set \$err.*?call \$__sura_string_concat.*?return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstSuperMethodThrower_catch_super_function_value.*?call \$__sura_method_AstMethodThrower_throw_function_value\s+local\.set \$__sura_wasm_call_tmp\s+global\.get \$__sura_exception_thrown\s+if.*?global\.get \$__sura_exception_value\s+local\.set \$__try_value\d+.*?br \$__try_end\d+.*?local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.get \$err.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy.*?i32\.const 20\s+(call \$block_double_ast|i32\.const 2\s+i32\.mul).*?return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstSuperMethodThrower_super_echo_label.*?call \$__sura_method_AstMethodThrower_echo_arg\s+local\.set \$__sura_wasm_call_tmp.*?local\.set \$echoed.*?local\.get \$echoed.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name.*?local\.get \$echoed.*?call \$__sura_string_concat.*?return' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?local\.set \$try_bool_payload' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?call \$__sura_string_concat.*?local\.set \$try_nil_payload' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_value_array.*?call \$__sura_value_is_truthy.*?local\.set \$try_array_payload_flag' -or
        $astWatText -notmatch '(?s)local\.get \$__try_value\d+\s+local\.set \$err.*?local\.get \$err.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy.*?local\.set \$try_dict_payload_flag' -or
        $astWatText -notmatch '(?s)local\.get \$inner\s+call \$__sura_value_num\s+local\.tee \$__try_value_tagged\d+\s+call \$__sura_value_payload\s+local\.set \$__try_value\d+\s+i32\.const 1\s+local\.set \$__try_thrown\d+\s+br \$__try_end\d+' -or
        $astWatText -notmatch 'br \$__try_end' -or
        $astWatText -notmatch 'local\.set \$score' -or
        $astWatText -notmatch 'local\.set \$label' -or
        $astWatText -notmatch 'local\.set \$greeting' -or
        $astWatText -notmatch 'local\.set \$nested_function_result' -or
        $astWatText -notmatch 'local\.set \$nested_function_type' -or
        $astWatText -notmatch 'local\.set \$nested_method_result' -or
        $astWatText -notmatch 'local\.set \$nested_method_type' -or
        $astWatText -notmatch 'local\.set \$if_local_method_result' -or
        $astWatText -notmatch 'local\.set \$if_local_method_type' -or
        $astWatText -notmatch 'local\.set \$match_local_method_result' -or
        $astWatText -notmatch 'local\.set \$match_local_method_type' -or
        $astWatText -notmatch 'local\.set \$score_label' -or
        $astWatText -notmatch 'local\.set \$flag_label' -or
        $astWatText -notmatch 'local\.set \$array_label' -or
        $astWatText -notmatch 'local\.set \$push_values' -or
        $astWatText -notmatch 'local\.set \$push_len' -or
        $astWatText -notmatch 'local\.set \$push_tail' -or
        $astWatText -notmatch 'local\.set \$push_label' -or
        $astWatText -notmatch 'local\.set \$pop_scores' -or
        $astWatText -notmatch 'local\.set \$pop_tail' -or
        $astWatText -notmatch 'local\.set \$pop_len' -or
        $astWatText -notmatch 'local\.set \$pop_name' -or
        $astWatText -notmatch 'local\.set \$pop_flag' -or
        $astWatText -notmatch 'local\.set \$pop_label' -or
        $astWatText -notmatch 'local\.set \$reverse_result' -or
        $astWatText -notmatch 'local\.set \$reverse_values_label' -or
        $astWatText -notmatch 'local\.set \$reverse_result_label' -or
        $astWatText -notmatch 'local\.set \$reverse_names_result' -or
        $astWatText -notmatch 'local\.set \$reverse_names_label' -or
        $astWatText -notmatch 'local\.set \$reverse_flags_label' -or
        $astWatText -notmatch 'local\.set \$reverse_label' -or
        $astWatText -notmatch 'local\.set \$sort_values' -or
        $astWatText -notmatch 'local\.set \$sort_result' -or
        $astWatText -notmatch 'local\.set \$sort_values_label' -or
        $astWatText -notmatch 'local\.set \$sort_result_label' -or
        $astWatText -notmatch 'local\.set \$sort_module_values' -or
        $astWatText -notmatch 'local\.set \$sort_module_result' -or
        $astWatText -notmatch 'local\.set \$sort_module_label' -or
        $astWatText -notmatch 'local\.set \$sort_label' -or
        $astWatText -notmatch 'local\.set \$repeat_values' -or
        $astWatText -notmatch 'local\.set \$repeat_values_label' -or
        $astWatText -notmatch 'local\.set \$repeat_names' -or
        $astWatText -notmatch 'local\.set \$repeat_names_label' -or
        $astWatText -notmatch 'local\.set \$repeat_flags' -or
        $astWatText -notmatch 'local\.set \$repeat_flags_label' -or
        $astWatText -notmatch 'local\.set \$repeat_empty' -or
        $astWatText -notmatch 'local\.set \$repeat_empty_len' -or
        $astWatText -notmatch 'local\.set \$repeat_label' -or
        $astWatText -notmatch 'local\.set \$unique_values' -or
        $astWatText -notmatch 'local\.set \$unique_values_label' -or
        $astWatText -notmatch 'local\.set \$unique_module_values' -or
        $astWatText -notmatch 'local\.set \$unique_module_label' -or
        $astWatText -notmatch 'local\.set \$unique_method_values' -or
        $astWatText -notmatch 'local\.set \$unique_method_result' -or
        $astWatText -notmatch 'local\.set \$unique_method_label' -or
        $astWatText -notmatch 'local\.set \$unique_label' -or
        $astWatText -notmatch 'local\.set \$set_union_values' -or
        $astWatText -notmatch 'local\.set \$set_union_values_label' -or
        $astWatText -notmatch 'local\.set \$set_union_module_values' -or
        $astWatText -notmatch 'local\.set \$set_union_module_label' -or
        $astWatText -notmatch 'local\.set \$set_union_single' -or
        $astWatText -notmatch 'local\.set \$set_union_single_label' -or
        $astWatText -notmatch 'local\.set \$set_union_label' -or
        $astWatText -notmatch 'local\.set \$set_intersection_values' -or
        $astWatText -notmatch 'local\.set \$set_intersection_values_label' -or
        $astWatText -notmatch 'local\.set \$set_intersection_module_values' -or
        $astWatText -notmatch 'local\.set \$set_intersection_module_label' -or
        $astWatText -notmatch 'local\.set \$set_difference_values' -or
        $astWatText -notmatch 'local\.set \$set_difference_values_label' -or
        $astWatText -notmatch 'local\.set \$set_difference_module_values' -or
        $astWatText -notmatch 'local\.set \$set_difference_module_label' -or
        $astWatText -notmatch 'local\.set \$set_symdiff_values' -or
        $astWatText -notmatch 'local\.set \$set_symdiff_values_label' -or
        $astWatText -notmatch 'local\.set \$set_symdiff_module_values' -or
        $astWatText -notmatch 'local\.set \$set_symdiff_module_label' -or
        $astWatText -notmatch 'local\.set \$set_symdiff_alias_values' -or
        $astWatText -notmatch 'local\.set \$set_symdiff_alias_label' -or
        $astWatText -notmatch 'local\.set \$set_subset_true' -or
        $astWatText -notmatch 'local\.set \$set_subset_false' -or
        $astWatText -notmatch 'local\.set \$set_subset_alias' -or
        $astWatText -notmatch 'local\.set \$set_superset_true' -or
        $astWatText -notmatch 'local\.set \$set_superset_false' -or
        $astWatText -notmatch 'local\.set \$set_superset_alias' -or
        $astWatText -notmatch 'local\.set \$set_interdiff_label' -or
        $astWatText -notmatch 'local\.set \$clone_source' -or
        $astWatText -notmatch 'local\.set \$clone_result' -or
        $astWatText -notmatch 'local\.set \$clone_copy' -or
        $astWatText -notmatch 'local\.set \$clone_source_label' -or
        $astWatText -notmatch 'local\.set \$clone_result_label' -or
        $astWatText -notmatch 'local\.set \$clone_copy_label' -or
        $astWatText -notmatch 'local\.set \$clone_label' -or
        $astWatText -notmatch 'local\.set \$concat_left' -or
        $astWatText -notmatch 'local\.set \$concat_right' -or
        $astWatText -notmatch 'local\.set \$concat_tail' -or
        $astWatText -notmatch 'local\.set \$concat_result' -or
        $astWatText -notmatch 'local\.set \$concat_left_label' -or
        $astWatText -notmatch 'local\.set \$concat_right_label' -or
        $astWatText -notmatch 'local\.set \$concat_result_label' -or
        $astWatText -notmatch 'local\.set \$concat_names' -or
        $astWatText -notmatch 'local\.set \$concat_names_label' -or
        $astWatText -notmatch 'local\.set \$concat_label' -or
        $astWatText -notmatch 'local\.set \$chunk_values' -or
        $astWatText -notmatch 'local\.set \$chunk_count' -or
        $astWatText -notmatch 'local\.set \$chunk_first' -or
        $astWatText -notmatch 'local\.set \$chunk_second' -or
        $astWatText -notmatch 'local\.set \$chunk_last' -or
        $astWatText -notmatch 'local\.set \$chunk_first_len' -or
        $astWatText -notmatch 'local\.set \$chunk_second_len' -or
        $astWatText -notmatch 'local\.set \$chunk_last_len' -or
        $astWatText -notmatch 'local\.set \$chunk_first_item' -or
        $astWatText -notmatch 'local\.set \$chunk_second_item' -or
        $astWatText -notmatch 'local\.set \$chunk_last_item' -or
        $astWatText -notmatch 'local\.set \$chunk_empty' -or
        $astWatText -notmatch 'local\.set \$chunk_empty_len' -or
        $astWatText -notmatch 'local\.set \$chunk_label' -or
        $astWatText -notmatch 'local\.set \$zip_values' -or
        $astWatText -notmatch 'local\.set \$zip_count' -or
        $astWatText -notmatch 'local\.set \$zip_first' -or
        $astWatText -notmatch 'local\.set \$zip_second' -or
        $astWatText -notmatch 'local\.set \$zip_first_len' -or
        $astWatText -notmatch 'local\.set \$zip_second_len' -or
        $astWatText -notmatch 'local\.set \$zip_first_left' -or
        $astWatText -notmatch 'local\.set \$zip_first_right' -or
        $astWatText -notmatch 'local\.set \$zip_second_left' -or
        $astWatText -notmatch 'local\.set \$zip_second_right' -or
        $astWatText -notmatch 'local\.set \$zip_empty' -or
        $astWatText -notmatch 'local\.set \$zip_empty_len' -or
        $astWatText -notmatch 'local\.set \$zip_label' -or
        $astWatText -notmatch 'local\.set \$flatten_chunks' -or
        $astWatText -notmatch 'local\.set \$flatten_chunks_label' -or
        $astWatText -notmatch 'local\.set \$flatten_zip' -or
        $astWatText -notmatch 'local\.set \$flatten_zip_label' -or
        $astWatText -notmatch 'local\.set \$flatten_method_source' -or
        $astWatText -notmatch 'local\.set \$flatten_method_values' -or
        $astWatText -notmatch 'local\.set \$flatten_method_label' -or
        $astWatText -notmatch 'local\.set \$flatten_literal' -or
        $astWatText -notmatch 'local\.set \$flatten_literal_label' -or
        $astWatText -notmatch 'local\.set \$flatten_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_source' -or
        $astWatText -notmatch 'local\.set \$dict_api_keys' -or
        $astWatText -notmatch 'local\.set \$dict_api_values' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_keys' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_values' -or
        $astWatText -notmatch 'local\.set \$dict_api_items' -or
        $astWatText -notmatch 'local\.set \$dict_api_item_first' -or
        $astWatText -notmatch 'local\.set \$dict_api_item_second' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_item' -or
        $astWatText -notmatch 'local\.set \$dict_api_items_len' -or
        $astWatText -notmatch 'local\.set \$dict_api_item_first_len' -or
        $astWatText -notmatch 'local\.set \$dict_api_item_second_len' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_item_len' -or
        $astWatText -notmatch 'local\.set \$dict_api_item_first_value' -or
        $astWatText -notmatch 'local\.set \$dict_api_item_second_value' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_item_value' -or
        $astWatText -notmatch 'local\.set \$dict_api_keys_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_values_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_keys_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_values_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_pick_source' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_method' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_pick' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_keys' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_values' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_keys_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_values_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_gamma' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_alpha' -or
        $astWatText -notmatch 'local\.set \$dict_api_picked_method_beta' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_pick_right' -or
        $astWatText -notmatch 'local\.set \$dict_api_pick_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_omit_source' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_method' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_omit' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_keys' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_values' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_keys_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_values_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_alpha' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_gamma' -or
        $astWatText -notmatch 'local\.set \$dict_api_omitted_method_beta' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_omit_right' -or
        $astWatText -notmatch 'local\.set \$dict_api_omit_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_merge_left' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_method' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_merge' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_keys' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_values' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_keys_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_values_label' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_alpha' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_beta' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_gamma' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_delta' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_method_beta' -or
        $astWatText -notmatch 'local\.set \$dict_api_merged_method_omega' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_merge_left' -or
        $astWatText -notmatch 'local\.set \$dict_api_direct_merge_right' -or
        $astWatText -notmatch 'local\.set \$dict_api_merge_label' -or
        $astWatText -notmatch 'local\.set \$dict_path_source' -or
        $astWatText -notmatch 'local\.set \$dict_path_hp' -or
        $astWatText -notmatch 'local\.set \$dict_path_name' -or
        $astWatText -notmatch 'local\.set \$dict_path_name_method' -or
        $astWatText -notmatch 'local\.set \$dict_path_missing' -or
        $astWatText -notmatch 'local\.set \$dict_path_label' -or
        $astWatText -notmatch 'local\.set \$json_path_hp' -or
        $astWatText -notmatch 'local\.set \$json_path_name' -or
        $astWatText -notmatch 'local\.set \$json_path_name_method' -or
        $astWatText -notmatch 'local\.set \$json_has_hp' -or
        $astWatText -notmatch 'local\.set \$json_has_missing' -or
        $astWatText -notmatch 'local\.set \$json_path_label' -or
        $astWatText -notmatch 'local\.set \$clear_values' -or
        $astWatText -notmatch 'local\.set \$clear_result' -or
        $astWatText -notmatch 'local\.set \$clear_len' -or
        $astWatText -notmatch 'local\.set \$clear_label' -or
        $astWatText -notmatch 'local\.set \$insert_values' -or
        $astWatText -notmatch 'local\.set \$insert_label' -or
        $astWatText -notmatch 'local\.set \$insert_front' -or
        $astWatText -notmatch 'local\.set \$insert_front_label' -or
        $astWatText -notmatch 'local\.set \$insert_tail' -or
        $astWatText -notmatch 'local\.set \$insert_tail_label' -or
        $astWatText -notmatch 'local\.set \$remove_values' -or
        $astWatText -notmatch 'local\.set \$removed_mid' -or
        $astWatText -notmatch 'local\.set \$remove_after_mid_label' -or
        $astWatText -notmatch 'local\.set \$removed_tail' -or
        $astWatText -notmatch 'local\.set \$remove_after_tail_label' -or
        $astWatText -notmatch 'local\.set \$removed_missing' -or
        $astWatText -notmatch 'local\.set \$remove_missing_len' -or
        $astWatText -notmatch 'local\.set \$remove_label' -or
        $astWatText -notmatch 'local\.set \$join_names_label' -or
        $astWatText -notmatch 'local\.set \$join_scores_label' -or
        $astWatText -notmatch 'local\.set \$join_flags_label' -or
        $astWatText -notmatch 'local\.set \$join_nils_label' -or
        $astWatText -notmatch 'local\.set \$join_label' -or
        $astWatText -notmatch 'local\.set \$slice_scores' -or
        $astWatText -notmatch 'local\.set \$slice_names' -or
        $astWatText -notmatch 'local\.set \$slice_flags' -or
        $astWatText -notmatch 'local\.set \$slice_nils' -or
        $astWatText -notmatch 'local\.set \$slice_scores_label' -or
        $astWatText -notmatch 'local\.set \$slice_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_name_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_bool_label' -or
        $astWatText -notmatch 'local\.set \$profile_dynamic_label' -or
        $astWatText -notmatch 'local\.set \$profile_has_name' -or
        $astWatText -notmatch 'local\.set \$dict_has_label' -or
        $astWatText -notmatch 'local\.set \$type_label' -or
        $astWatText -notmatch 'local\.set \$function_alias' -or
        $astWatText -notmatch 'local\.set \$function_type_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_function_param_call_a' -or
        $astWatText -notmatch 'local\.set \$dynamic_function_param_call_b' -or
        $astWatText -notmatch 'local\.set \$dynamic_local_function_call_a' -or
        $astWatText -notmatch 'local\.set \$dynamic_local_function_call_b' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_function_call_a' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_function_call_b' -or
        $astWatText -notmatch 'local\.set \$local_function_value_label' -or
        $astWatText -notmatch 'local\.set \$control_flow_function_alias_label' -or
        $astWatText -notmatch 'local\.set \$function_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_alias_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_index_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_dynamic_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_dynamic_index_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_handlers' -or
        $astWatText -notmatch 'local\.set \$function_map' -or
        $astWatText -notmatch 'local\.set \$function_idx' -or
        $astWatText -notmatch 'local\.set \$function_key_lookup' -or
        $astWatText -notmatch 'local\.set \$function_update_key' -or
        $astWatText -notmatch 'local\.set \$dynamic_tagged_update_key' -or
        $astWatText -notmatch 'local\.set \$mixed_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_dynamic_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_string_label' -or
        $astWatText -notmatch 'local\.set \$mixed_num_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_num_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_num_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_num_calc' -or
        $astWatText -notmatch 'local\.set \$mixed_bool_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_bool_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_bool_label' -or
        $astWatText -notmatch 'local\.set \$mixed_nil_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_nil_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_nil_label' -or
        $astWatText -notmatch 'local\.set \$mixed_object_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_object_dict' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_exact_dict_label' -or
        $astWatText -notmatch 'local\.set \$mixed_array_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_array_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_array_label' -or
        $astWatText -notmatch 'local\.set \$mixed_nested_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_nested_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_dict_label' -or
        $astWatText -notmatch 'local\.set \$mixed_function_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_function_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_function_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_function_same' -or
        $astWatText -notmatch 'local\.set \$function_choice' -or
        $astWatText -notmatch 'local\.set \$function_pick_call' -or
        $astWatText -notmatch 'local\.set \$function_pick_true' -or
        $astWatText -notmatch 'local\.set \$function_pick_true_call' -or
        $astWatText -notmatch 'local\.set \$function_match_pick' -or
        $astWatText -notmatch 'local\.set \$function_match_pick_call' -or
        $astWatText -notmatch 'local\.set \$function_passed' -or
        $astWatText -notmatch 'local\.set \$function_param_call' -or
        $astWatText -notmatch 'local\.set \$function_param_call_label' -or
        $astWatText -notmatch 'local\.set \$function_lookup_label' -or
        $astWatText -notmatch 'local\.set \$function_param_label' -or
        $astWatText -notmatch 'local\.set \$function_holder_label' -or
        $astWatText -notmatch 'local\.set \$function_holder_call' -or
        $astWatText -notmatch 'local\.set \$function_holder_alt_call' -or
        $astWatText -notmatch 'local\.set \$function_holder_returned_handler' -or
        $astWatText -notmatch 'local\.set \$function_holder_return_call' -or
        $astWatText -notmatch 'local\.set \$function_holder_chosen_handler' -or
        $astWatText -notmatch 'local\.set \$function_holder_chosen_call' -or
        $astWatText -notmatch 'local\.set \$super_object_param_type_label' -or
        $astWatText -notmatch 'local\.set \$super_object_param_array_len_label' -or
        $astWatText -notmatch 'local\.set \$function_holder_match_handler' -or
        $astWatText -notmatch 'local\.set \$function_holder_match_call' -or
        $astWatText -notmatch 'local\.set \$function_child_label' -or
        $astWatText -notmatch 'local\.set \$function_child_inline_handler' -or
        $astWatText -notmatch 'local\.set \$function_child_inline_call' -or
        $astWatText -notmatch 'local\.set \$function_child_inline_label' -or
        $astWatText -notmatch 'local\.set \$function_child_inline_choice_label' -or
        $astWatText -notmatch 'local\.set \$function_foreach_label' -or
        $astWatText -notmatch 'local\.set \$function_foreach_dict_label' -or
        $astWatText -notmatch 'local\.set \$object_foreach_label' -or
        $astWatText -notmatch 'local\.set \$object_foreach_dict_label' -or
        $astWatText -notmatch 'local\.set \$object_foreach_return_call_label' -or
        $astWatText -notmatch 'local\.set \$object_foreach_dict_return_call_label' -or
        $astWatText -notmatch 'local\.set \$object_param_method_label' -or
        $astWatText -notmatch 'local\.set \$object_param_field_label' -or
        $astWatText -notmatch 'local\.set \$object_param_index_label' -or
        $astWatText -notmatch 'local\.set \$object_function_param_field_label' -or
        $astWatText -notmatch 'local\.set \$object_function_param_index_label' -or
        $astWatText -notmatch 'local\.set \$object_ctor_field_method_label' -or
        $astWatText -notmatch 'local\.set \$object_ctor_field_label' -or
        $astWatText -notmatch 'local\.set \$object_ctor_index_label' -or
        $astWatText -notmatch 'local\.set \$object_super_ctor_method_label' -or
        $astWatText -notmatch 'local\.set \$object_super_ctor_child_method_label' -or
        $astWatText -notmatch 'local\.set \$object_super_ctor_field_label' -or
        $astWatText -notmatch 'local\.set \$object_super_ctor_index_label' -or
        $astWatText -notmatch 'local\.set \$function_truth_score' -or
        $astWatText -notmatch 'local\.set \$len_label' -or
        $astWatText -notmatch 'local\.set \$len_mixed_ternary_label' -or
        $astWatText -notmatch 'local\.set \$str_label' -or
        $astWatText -notmatch 'local\.set \$literal_array_to_str_label' -or
        $astWatText -notmatch 'local\.set \$literal_dict_to_str_label' -or
        $astWatText -notmatch 'local\.set \$runtime_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$ternary_literal_to_str_label' -or
        $astWatText -notmatch 'local\.set \$array_var_to_str_label' -or
        $astWatText -notmatch 'local\.set \$mixed_array_var_to_str_label' -or
        $astWatText -notmatch 'local\.set \$mixed_array_index_to_str_label' -or
        $astWatText -notmatch 'local\.set \$mixed_array_index_runtime_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_array_index_label' -or
        $astWatText -notmatch 'local\.set \$mixed_values_from_func' -or
        $astWatText -notmatch 'local\.set \$mixed_function_return_array_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_array_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_array_to_str_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_array_interp_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_array_concat_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_collection_runtime_label' -or
        $astWatText -notmatch 'local\.set \$param_return_collection_runtime_label' -or
        $astWatText -notmatch 'local\.set \$function_return_choice_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_return_local_choice_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_return_choice_access_label' -or
        $astWatText -notmatch 'local\.set \$function_return_local_choice_access_label' -or
        $astWatText -notmatch 'local\.set \$function_return_alias_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$function_return_alias_access_label' -or
        $astWatText -notmatch 'local\.set \$function_return_local_scope_alias_label' -or
        $astWatText -notmatch 'local\.set \$method_return_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$method_return_choice_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$method_return_local_choice_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$method_return_choice_access_label' -or
        $astWatText -notmatch 'local\.set \$method_return_local_choice_access_label' -or
        $astWatText -notmatch 'local\.set \$constructor_param_collection_label' -or
        $astWatText -notmatch 'local\.set \$super_constructor_param_collection_label' -or
        $astWatText -notmatch 'local\.set \$super_method_param_collection_label' -or
        $astWatText -notmatch 'local\.set \$super_method_return_collection_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_dict_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_dict_to_str_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_dict_interp_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_dict_concat_label' -or
        $astWatText -notmatch 'local\.set \$direct_mixed_function_return_dot_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_dict_index_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_dot_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_access_interp_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_access_call_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_access_eq_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_access_numeric_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_access_numeric_ternary_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_access_numeric_coalesce_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_coalesce_receiver_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_nested_access_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_same_dict_shape_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_same_array_shape_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_coalesce_shape_label' -or
        $astWatText -notmatch 'local\.set \$returned_inline_function_label' -or
        $astWatText -notmatch 'local\.set \$captured_inline_function_param_call' -or
        $astWatText -notmatch 'local\.set \$returned_param_function_capture_label' -or
        $astWatText -notmatch 'local\.set \$captured_inline_function_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_string_method_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_string_chain_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_string_call_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_string_search_alias_variant_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_len_method_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_alias_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_alias_method_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_collection_alias_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_collection_len_method_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_number_collection_alias_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_mutable_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_array_module_alias_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_array_direct_alias_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_array_alias_variant_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_return_alias_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_return_mutable_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_method_mutable_label' -or
        $astWatText -notmatch 'local\.set \$value_runtime_super_mutable_label' -or
        $astWatText -notmatch 'local\.set \$mixed_dict_index_runtime_label' -or
        $astWatText -notmatch 'local\.set \$mixed_exact_not_label' -or
        $astWatText -notmatch 'local\.set \$mixed_exact_logic_label' -or
        $astWatText -notmatch 'local\.set \$mixed_exact_eq_label' -or
        $astWatText -notmatch 'local\.set \$mixed_exact_compare_label' -or
        $astWatText -notmatch 'local\.set \$mixed_exact_arith_label' -or
        $astWatText -notmatch 'local\.set \$mixed_exact_bitwise_label' -or
        $astWatText -notmatch 'local\.set \$dict_var_to_str_label' -or
        $astWatText -notmatch 'local\.set \$nested_collection_to_str_label' -or
        $astWatText -notmatch 'local\.set \$to_str_mixed_ternary_label' -or
        $astWatText -notmatch 'local\.set \$type_mixed_ternary_label' -or
        $astWatText -notmatch 'local\.set \$bool_label' -or
        $astWatText -notmatch '(?s)i32\.const \d+\s+local\.set \$function_alias' -or
        $astWatText -notmatch '(?s)i32\.const \d+.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.get \$function_alias.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?i32\.const \d+.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy.*?local\.set \$function_type_label' -or
        $astWatText -notmatch '(?s)i32\.const 70.*?call \$__sura_make_array_23\s+call \$__sura_string_concat\s+local\.set \$function_to_str_label' -or
        $astWatText -notmatch '(?s)i32\.const 65.*?call \$__sura_make_array_23\s+call \$__sura_string_concat\s+local\.set \$function_alias_to_str_label' -or
        $astWatText -notmatch '(?s)i32\.const 70.*?call \$__sura_make_array_23\s+call \$__sura_string_concat.*?local\.set \$function_index_to_str_label' -or
        $astWatText -notmatch '(?s)i32\.const 70.*?call \$__sura_make_array_23\s+call \$__sura_string_concat.*?local\.set \$function_dynamic_to_str_label' -or
        $astWatText -notmatch '(?s)i32\.const 70.*?call \$__sura_make_array_22\s+call \$__sura_string_concat.*?local\.set \$function_dynamic_index_to_str_label' -or
        $astWatText -notmatch '(?s)i32\.const \d+.*?call \$__sura_value_function.*?local\.get \$function_alias.*?call \$__sura_value_function.*?call \$__sura_value_eq' -or
        $astWatText -notmatch '(?s)\(func \$nested_label_pair.*?call \$label_pair\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$inner\s+local\.get \$inner\s+return' -or
        $astWatText -notmatch '(?s)call \$nested_label_pair\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name\s+local\.set \$nested_function_type' -or
        $astWatText -notmatch '(?s)\(func \$nested_method_label.*?call \$__sura_new_AstMethodThrower.*?local\.set \$local_thrower.*?call \$__sura_method_AstMethodThrower_echo_arg.*?local\.set \$echoed.*?local\.get \$echoed\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name.*?local\.get \$echoed.*?call \$__sura_string_concat\s+return' -or
        $astWatText -notmatch '(?s)call \$nested_method_label\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name\s+local\.set \$nested_method_type' -or
        $astWatText -notmatch '(?s)\(func \$if_local_method_label.*?call \$__sura_new_AstMethodThrower.*?local\.set \$selected.*?call \$__sura_method_AstMethodThrower_echo_arg.*?local\.set \$echoed.*?local\.get \$echoed\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name.*?local\.get \$echoed.*?call \$__sura_string_concat\s+return' -or
        $astWatText -notmatch '(?s)call \$if_local_method_label\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name\s+local\.set \$if_local_method_type' -or
        $astWatText -notmatch '(?s)\(func \$match_local_method_label.*?call \$__sura_new_AstMethodThrower.*?local\.set \$selected.*?call \$__sura_method_AstMethodThrower_echo_arg.*?local\.set \$echoed.*?local\.get \$echoed\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name.*?local\.get \$echoed.*?call \$__sura_string_concat\s+return' -or
        $astWatText -notmatch '(?s)call \$match_local_method_label\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name\s+local\.set \$match_local_method_type' -or
        $astWatText -notmatch '(?s)\(func \$choose_tagged_if_ast.*?if.*?call \$__sura_new_AstTagged.*?local\.get \$__sura_wasm_call_tmp\s+return.*?else.*?call \$__sura_new_AstTagged.*?local\.get \$__sura_wasm_call_tmp\s+return' -or
        $astWatText -notmatch '(?s)\(func \$local_tagged_from_if_len_ast.*?call \$choose_tagged_if_ast.*?local\.set \$bot.*?call \$__sura_method_AstTagged_kind_text.*?local\.set \$label.*?local\.get \$label\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+return' -or
        $astWatText -notmatch '(?s)\(func \$choose_tagged_match_ast.*?call \$__sura_new_AstTagged.*?local\.get \$__sura_wasm_call_tmp\s+return.*?call \$__sura_new_AstTagged.*?local\.get \$__sura_wasm_call_tmp\s+return' -or
        $astWatText -notmatch '(?s)\(func \$local_tagged_from_match_len_ast.*?call \$choose_tagged_match_ast.*?local\.set \$bot.*?call \$__sura_method_AstTagged_kind_text.*?local\.set \$label.*?local\.get \$label\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+return' -or
        $astWatText -notmatch '(?s)\(func \$describe_function_ast.*?local\.get \$handler.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.get \$handler.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy' -or
        $astWatText -notmatch '(?s)\(func \$pass_function_ast.*?local\.get \$handler\s+return' -or
        $astWatText -notmatch '(?s)\(func \$pick_function_ast.*?if \(result i32\).*?i32\.const \d+.*?else.*?i32\.const \d+.*?end\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstFunctionHolder_handler_value.*?call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstFunctionHolder_handler_truth.*?call \$__sura_dict_get.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstFunctionChild_inherited_handler.*?call \$__sura_method_AstFunctionHolder_handler_value.*?local\.get \$__sura_wasm_call_tmp\s+return' -or
        $astWatText -notmatch '(?s)local\.set \$handler.*?local\.get \$function_foreach_label\s+local\.get \$handler.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.set \$function_foreach_label' -or
        $astWatText -notmatch '(?s)local\.set \$tagged_item.*?local\.get \$tagged_item\s+call \$__sura_method_AstTagged_kind_text.*?call \$__sura_string_concat.*?local\.set \$object_foreach_label' -or
        $astWatText -notmatch '(?s)local\.set \$tagged_value.*?local\.get \$tagged_value\s+call \$__sura_method_AstTagged_kind_text.*?call \$__sura_string_concat.*?local\.set \$object_foreach_dict_label' -or
        $astWatText -notmatch '(?s)\(func \$foreach_return_tagged_ast.*?local\.set \$local_tagged_item.*?local\.get \$local_tagged_item\s+return' -or
        $astWatText -notmatch '(?s)\(func \$foreach_return_tagged_dict_ast.*?local\.set \$local_tagged_value.*?local\.get \$local_tagged_value\s+return' -or
        $astWatText -notmatch '(?s)call \$foreach_return_tagged_ast.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_foreach_return_call_label' -or
        $astWatText -notmatch '(?s)call \$foreach_return_tagged_dict_ast.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_foreach_dict_return_call_label' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTagged_peer_kind_label.*?local\.get \$other\s+call \$__sura_method_AstTagged_kind_text.*?local\.get \$__sura_wasm_call_tmp\s+return' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTagged_peer_kind_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_param_method_label' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTagged_peer_kind_field_label.*?local\.get \$other\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTagged_peer_kind_index_label.*?local\.get \$other\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTagged_peer_kind_field_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_param_field_label' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTagged_peer_kind_index_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_param_index_label' -or
        $astWatText -notmatch '(?s)\(func \$tagged_param_field_label_ast.*?local\.get \$bot\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)\(func \$tagged_param_index_label_ast.*?local\.get \$bot\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)call \$tagged_param_field_label_ast\s+.*?call \$__sura_string_concat\s+local\.set \$object_function_param_field_label' -or
        $astWatText -notmatch '(?s)call \$tagged_param_index_label_ast\s+.*?call \$__sura_string_concat\s+local\.set \$object_function_param_index_label' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTaggedBox_peer_kind_label.*?local\.get \$self\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.get \$__sura_wasm_call_tmp\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTaggedBox_peer_kind_field_label.*?local\.get \$self\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTaggedBox_peer_kind_index_label.*?local\.get \$self\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBox_peer_kind_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_ctor_field_method_label' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBox_peer_kind_field_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_ctor_field_label' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBox_peer_kind_index_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_ctor_index_label' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBox_peer_kind_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_super_ctor_method_label' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTaggedBoxChild_child_peer_kind_label.*?local\.get \$self\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.get \$__sura_wasm_call_tmp\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTaggedBoxChild_child_peer_kind_field_label.*?local\.get \$self\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)\(func \$__sura_method_AstTaggedBoxChild_child_peer_kind_index_label.*?local\.get \$self\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+return' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBoxChild_child_peer_kind_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_super_ctor_child_method_label' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBoxChild_child_peer_kind_field_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_super_ctor_field_label' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstTaggedBoxChild_child_peer_kind_index_label\s+.*?call \$__sura_string_concat\s+local\.set \$object_super_ctor_index_label' -or
        $astWatText -notmatch '(?s)local\.get \$function_alias.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy.*?local\.set \$function_truth_score' -or
        $astWatText -notmatch '(?s)local\.get \$function_handlers\s+(?:local\.get \$function_idx|i32\.const 1)\s+call \$__sura_array_get_checked.*?call \$__sura_value_function' -or
        $astWatText -notmatch '(?s)local\.get \$function_map\s+local\.get \$function_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_function' -or
        $astWatText -notmatch '(?s)local\.get \$function_map\s+local\.get \$function_update_key\s+i32\.const \d+\s+call \$__sura_dict_put\s+local\.set \$function_map' -or
        $astWatText -notmatch '(?s)local\.get \$function_holder.*?call \$__sura_make_array_\d+\s+i32\.const \d+\s+call \$__sura_dict_put\s+local\.set \$function_holder' -or
        $astWatText -notmatch '(?s)local\.get \$tagged_objects\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_index_label' -or
        $astWatText -notmatch '(?s)local\.get \$tagged_map\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_dot_label' -or
        $astWatText -notmatch '(?s)local\.get \$tagged_map\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_key_label' -or
        $astWatText -notmatch '(?s)local\.get \$tagged_objects\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_dynamic_index_label' -or
        $astWatText -notmatch '(?s)local\.get \$tagged_map\s+local\.get \$tagged_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_dynamic_key_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_map\s+local\.get \$dynamic_tagged_update_key\s+call \$__sura_new_AstTagged.*?call \$__sura_dict_put\s+local\.set \$dynamic_tagged_map' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_map\s+local\.get \$dynamic_tagged_update_key\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_dynamic_assigned_key_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_map\s+local\.get \$dynamic_tagged_update_key\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+.*?call \$__sura_string_concat\s+local\.set \$object_dynamic_assigned_field_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_map\s+local\.get \$dynamic_tagged_update_key\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+.*?call \$__sura_string_concat\s+local\.set \$object_dynamic_assigned_index_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_dynamic_dict\s+local\.get \$mixed_key_lookup.*?call \$__sura_dict_put\s+local\.set \$mixed_dynamic_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_dynamic_dict\s+local\.get \$mixed_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_string_concat\s+local\.set \$dynamic_dict_assigned_string_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_num_dict\s+local\.get \$mixed_num_key_lookup\s+local\.get \$score\s+call \$__sura_dict_put\s+local\.set \$mixed_num_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_num_dict\s+local\.get \$mixed_num_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string\s+call \$__sura_string_concat\s+local\.set \$dynamic_dict_assigned_num_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_num_dict\s+local\.get \$mixed_num_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 4\s+i32\.add\s+local\.set \$dynamic_dict_assigned_num_calc' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_bool_dict\s+local\.get \$mixed_bool_key_lookup\s+i32\.const 0\s+call \$__sura_dict_put\s+local\.set \$mixed_bool_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_bool_dict\s+local\.get \$mixed_bool_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+if \(result i32\).*?call \$__sura_string_concat\s+local\.set \$dynamic_dict_assigned_bool_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_nil_dict\s+local\.get \$mixed_nil_key_lookup\s+i32\.const 0\s+call \$__sura_dict_put\s+local\.set \$mixed_nil_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_object_dict\s+local\.get \$mixed_object_key_lookup\s+call \$__sura_new_AstTagged.*?call \$__sura_dict_put\s+local\.set \$mixed_object_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_object_dict\s+local\.get \$mixed_object_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_dynamic_exact_dict_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_array_dict\s+local\.get \$mixed_array_key_lookup.*?call \$__sura_dict_put\s+local\.set \$mixed_array_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_array_dict\s+local\.get \$mixed_array_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_array.*?call \$__sura_value_length.*?local\.get \$mixed_array_dict\s+local\.get \$mixed_array_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_array.*?call \$__sura_value_is_truthy.*?local\.set \$dynamic_dict_assigned_array_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_nested_dict\s+local\.get \$mixed_nested_key_lookup.*?call \$__sura_dict_put\s+local\.set \$mixed_nested_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_nested_dict\s+local\.get \$mixed_nested_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_dict.*?call \$__sura_value_length.*?local\.get \$mixed_nested_dict\s+local\.get \$mixed_nested_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy.*?local\.set \$dynamic_dict_assigned_dict_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_function_dict\s+local\.get \$mixed_function_key_lookup\s+i32\.const \d+\s+call \$__sura_dict_put\s+local\.set \$mixed_function_dict' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_function_dict\s+local\.get \$mixed_function_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_function.*?call \$__sura_value_type_name.*?local\.get \$mixed_function_dict\s+local\.get \$mixed_function_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_function.*?call \$__sura_value_is_truthy.*?local\.set \$dynamic_dict_assigned_function_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_function_dict\s+local\.get \$mixed_function_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_value_function.*?i32\.const \d+.*?call \$__sura_value_function.*?call \$__sura_value_eq\s+local\.set \$dynamic_dict_assigned_function_same' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_new_AstTagged.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$object_dynamic_assigned_array_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+i32\.const \d+\s+call \$__sura_dict_get\s+.*?call \$__sura_string_concat\s+local\.set \$object_dynamic_assigned_array_field_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_tagged_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+i32\.const \d+\s+call \$__sura_dict_get\s+.*?call \$__sura_string_concat\s+local\.set \$object_dynamic_assigned_array_index_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_string_items\s+(?:local\.get \$dynamic_idx|i32\.const 1).*?call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_string_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat\s+local\.set \$dynamic_assigned_string_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_bool_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+i32\.const 1\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_bool_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+if \(result i32\).*?call \$__sura_string_concat\s+local\.set \$dynamic_assigned_bool_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_number_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+local\.get \$score\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_number_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string\s+call \$__sura_string_concat\s+local\.set \$dynamic_assigned_num_label' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_number_items\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+i32\.const 4\s+i32\.add\s+local\.set \$dynamic_assigned_num_calc' -or
        $astWatText -notmatch '(?s)local\.get \$names\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked.*?call \$__sura_string_concat.*?local\.set \$dynamic_name_label' -or
        $astWatText -notmatch '(?s)local\.get \$push_values\s+i32\.const 3\s+call \$__sura_array_push\s+local\.set \$push_values' -or
        $astWatText -notmatch '(?s)local\.get \$push_values\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$push_len' -or
        $astWatText -notmatch '(?s)local\.get \$push_values\s+i32\.const 2\s+call \$__sura_array_get_checked\s+local\.set \$push_tail' -or
        $astWatText -notmatch '\(func \$__sura_array_pop' -or
        $astWatText -notmatch '(?s)local\.get \$pop_scores\s+call \$__sura_array_pop\s+local\.set \$pop_tail' -or
        $astWatText -notmatch '(?s)local\.get \$pop_scores\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$pop_len' -or
        $astWatText -notmatch '(?s)local\.get \$pop_names\s+call \$__sura_array_pop\s+local\.set \$pop_name' -or
        $astWatText -notmatch '(?s)local\.get \$pop_flags\s+call \$__sura_array_pop\s+local\.set \$pop_flag' -or
        $astWatText -notmatch '(?s)local\.get \$pop_name\s+.*?call \$__sura_string_concat.*?local\.get \$pop_flag\s+if \(result i32\).*?local\.set \$pop_label' -or
        $astWatText -notmatch '\(func \$__sura_array_reverse' -or
        $astWatText -notmatch '(?s)local\.get \$reverse_values\s+call \$__sura_array_reverse\s+local\.set \$reverse_result' -or
        $astWatText -notmatch '(?s)local\.get \$reverse_values\s+.*?call \$__sura_array_join_num\s+local\.set \$reverse_values_label' -or
        $astWatText -notmatch '(?s)local\.get \$reverse_result\s+.*?call \$__sura_array_join_num\s+local\.set \$reverse_result_label' -or
        $astWatText -notmatch '(?s)local\.get \$reverse_names\s+call \$__sura_array_reverse\s+local\.set \$reverse_names_result' -or
        $astWatText -notmatch '(?s)local\.get \$reverse_names_result\s+.*?call \$__sura_array_join_string\s+local\.set \$reverse_names_label' -or
        $astWatText -notmatch '(?s)local\.get \$reverse_flags\s+call \$__sura_array_reverse\s+drop\s+local\.get \$reverse_flags\s+.*?call \$__sura_array_join_bool\s+local\.set \$reverse_flags_label' -or
        $astWatText -notmatch '\(func \$__sura_array_sort' -or
        $astWatText -notmatch '(?s)local\.get \$sort_values\s+call \$__sura_array_sort\s+local\.set \$sort_result' -or
        $astWatText -notmatch '(?s)local\.get \$sort_values\s+.*?call \$__sura_array_join_num\s+local\.set \$sort_values_label' -or
        $astWatText -notmatch '(?s)local\.get \$sort_result\s+.*?call \$__sura_array_join_num\s+local\.set \$sort_result_label' -or
        $astWatText -notmatch '(?s)local\.get \$sort_module_values\s+call \$__sura_array_sort\s+local\.set \$sort_module_result' -or
        $astWatText -notmatch '(?s)local\.get \$sort_module_result\s+.*?call \$__sura_array_join_num\s+local\.set \$sort_module_label' -or
        $astWatText -notmatch '\(func \$__sura_array_repeat' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+i32\.const 3\s+call \$__sura_array_repeat\s+local\.set \$repeat_values' -or
        $astWatText -notmatch '(?s)local\.get \$repeat_values\s+.*?call \$__sura_array_join_num\s+local\.set \$repeat_values_label' -or
        $astWatText -notmatch '(?s)i32\.const \d+.*?i32\.const 2\s+call \$__sura_array_repeat\s+local\.set \$repeat_names' -or
        $astWatText -notmatch '(?s)local\.get \$repeat_names\s+.*?call \$__sura_array_join_string\s+local\.set \$repeat_names_label' -or
        $astWatText -notmatch '(?s)i32\.const 1\s+i32\.const 2\s+call \$__sura_array_repeat\s+local\.set \$repeat_flags' -or
        $astWatText -notmatch '(?s)local\.get \$repeat_flags\s+.*?call \$__sura_array_join_bool\s+local\.set \$repeat_flags_label' -or
        $astWatText -notmatch '(?s)i32\.const 9\s+i32\.const 0\s+call \$__sura_array_repeat\s+local\.set \$repeat_empty' -or
        $astWatText -notmatch '(?s)local\.get \$repeat_empty\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$repeat_empty_len' -or
        $astWatText -notmatch '\(func \$__sura_array_unique' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+local\.set \$unique_values' -or
        $astWatText -notmatch '(?s)local\.get \$unique_values\s+.*?call \$__sura_array_join_num\s+local\.set \$unique_values_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+local\.set \$unique_module_values' -or
        $astWatText -notmatch '(?s)local\.get \$unique_module_values\s+.*?call \$__sura_array_join_num\s+local\.set \$unique_module_label' -or
        $astWatText -notmatch '(?s)local\.get \$unique_method_values\s+call \$__sura_array_unique\s+local\.set \$unique_method_result' -or
        $astWatText -notmatch '(?s)local\.get \$unique_method_result\s+.*?call \$__sura_array_join_num\s+local\.set \$unique_method_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_concat2\s+.*?call \$__sura_array_concat2\s+call \$__sura_array_unique\s+local\.set \$set_union_values' -or
        $astWatText -notmatch '(?s)local\.get \$set_union_values\s+.*?call \$__sura_array_join_num\s+local\.set \$set_union_values_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_concat2\s+.*?call \$__sura_array_concat2\s+call \$__sura_array_unique\s+local\.set \$set_union_module_values' -or
        $astWatText -notmatch '(?s)local\.get \$set_union_module_values\s+.*?call \$__sura_array_join_num\s+local\.set \$set_union_module_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+local\.set \$set_union_single' -or
        $astWatText -notmatch '\(func \$__sura_array_intersection2' -or
        $astWatText -notmatch '\(func \$__sura_array_difference2' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+.*?call \$__sura_array_intersection2\s+.*?call \$__sura_array_intersection2\s+local\.set \$set_intersection_values' -or
        $astWatText -notmatch '(?s)local\.get \$set_intersection_values\s+.*?call \$__sura_array_join_num\s+local\.set \$set_intersection_values_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+.*?call \$__sura_array_intersection2\s+local\.set \$set_intersection_module_values' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+.*?call \$__sura_array_difference2\s+.*?call \$__sura_array_difference2\s+local\.set \$set_difference_values' -or
        $astWatText -notmatch '(?s)local\.get \$set_difference_values\s+.*?call \$__sura_array_join_num\s+local\.set \$set_difference_values_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_unique\s+.*?call \$__sura_array_difference2\s+local\.set \$set_difference_module_values' -or
        $astWatText -notmatch '\(func \$__sura_array_symmetric_difference2' -or
        $astWatText -notmatch '\(func \$__sura_array_is_subset' -or
        $astWatText -notmatch '(?s)call \$__sura_array_symmetric_difference2\s+local\.set \$set_symdiff_values' -or
        $astWatText -notmatch '(?s)local\.get \$set_symdiff_values\s+.*?call \$__sura_array_join_num\s+local\.set \$set_symdiff_values_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_symmetric_difference2\s+local\.set \$set_symdiff_module_values' -or
        $astWatText -notmatch '(?s)call \$__sura_array_symmetric_difference2\s+local\.set \$set_symdiff_alias_values' -or
        $astWatText -notmatch '(?s)call \$__sura_array_is_subset\s+local\.set \$set_subset_true' -or
        $astWatText -notmatch '(?s)call \$__sura_array_is_subset\s+local\.set \$set_subset_false' -or
        $astWatText -notmatch '(?s)call \$__sura_array_is_subset\s+local\.set \$set_subset_alias' -or
        $astWatText -notmatch '(?s)call \$__sura_array_is_subset\s+local\.set \$set_superset_true' -or
        $astWatText -notmatch '\(func \$__sura_array_clone' -or
        $astWatText -notmatch '(?s)local\.get \$clone_source\s+call \$__sura_array_clone\s+local\.set \$clone_result' -or
        $astWatText -notmatch '(?s)local\.get \$clone_result\s+i32\.const 0\s+i32\.const 4\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$clone_source\s+call \$__sura_array_clone\s+local\.set \$clone_copy' -or
        $astWatText -notmatch '(?s)local\.get \$clone_source\s+.*?call \$__sura_array_join_num\s+local\.set \$clone_source_label' -or
        $astWatText -notmatch '(?s)local\.get \$clone_result\s+.*?call \$__sura_array_join_num\s+local\.set \$clone_result_label' -or
        $astWatText -notmatch '(?s)local\.get \$clone_copy\s+.*?call \$__sura_array_join_num\s+local\.set \$clone_copy_label' -or
        $astWatText -notmatch '\(func \$__sura_array_concat2' -or
        $astWatText -notmatch '(?s)local\.get \$concat_left\s+local\.get \$concat_right\s+call \$__sura_array_concat2\s+local\.get \$concat_tail\s+call \$__sura_array_concat2\s+local\.set \$concat_result' -or
        $astWatText -notmatch '(?s)local\.get \$concat_result\s+i32\.const 1\s+i32\.const 9\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$concat_left\s+.*?call \$__sura_array_join_num\s+local\.set \$concat_left_label' -or
        $astWatText -notmatch '(?s)local\.get \$concat_right\s+.*?call \$__sura_array_join_num\s+local\.set \$concat_right_label' -or
        $astWatText -notmatch '(?s)local\.get \$concat_result\s+.*?call \$__sura_array_join_num\s+local\.set \$concat_result_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_concat2\s+local\.set \$concat_names' -or
        $astWatText -notmatch '(?s)local\.get \$concat_names\s+.*?call \$__sura_array_join_string\s+local\.set \$concat_names_label' -or
        $astWatText -notmatch '\(func \$__sura_array_chunk' -or
        $astWatText -notmatch '(?s)i32\.const 2\s+call \$__sura_array_chunk\s+local\.set \$chunk_values' -or
        $astWatText -notmatch '(?s)local\.get \$chunk_values\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$chunk_count' -or
        $astWatText -notmatch '(?s)local\.get \$chunk_values\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$chunk_first' -or
        $astWatText -notmatch '(?s)local\.get \$chunk_first\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$chunk_first_len' -or
        $astWatText -notmatch '(?s)local\.get \$chunk_first\s+i32\.const 1\s+call \$__sura_array_get_checked\s+local\.set \$chunk_first_item' -or
        $astWatText -notmatch '(?s)i32\.const 3\s+call \$__sura_array_chunk\s+local\.set \$chunk_empty' -or
        $astWatText -notmatch '\(func \$__sura_array_zip2' -or
        $astWatText -notmatch '(?s)call \$__sura_array_zip2\s+local\.set \$zip_values' -or
        $astWatText -notmatch '(?s)local\.get \$zip_values\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$zip_count' -or
        $astWatText -notmatch '(?s)local\.get \$zip_values\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$zip_first' -or
        $astWatText -notmatch '(?s)local\.get \$zip_first\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$zip_first_len' -or
        $astWatText -notmatch '(?s)local\.get \$zip_first\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$zip_first_left' -or
        $astWatText -notmatch '(?s)local\.get \$zip_first\s+i32\.const 1\s+call \$__sura_array_get_checked\s+local\.set \$zip_first_right' -or
        $astWatText -notmatch '(?s)call \$__sura_array_zip2\s+local\.set \$zip_empty' -or
        $astWatText -notmatch '\(func \$__sura_array_flatten1' -or
        $astWatText -notmatch '(?s)i32\.const 2\s+call \$__sura_array_chunk\s+call \$__sura_array_flatten1\s+local\.set \$flatten_chunks' -or
        $astWatText -notmatch '(?s)local\.get \$flatten_chunks\s+.*?call \$__sura_array_join_num\s+local\.set \$flatten_chunks_label' -or
        $astWatText -notmatch '(?s)call \$__sura_array_zip2\s+call \$__sura_array_flatten1\s+local\.set \$flatten_zip' -or
        $astWatText -notmatch '(?s)local\.get \$flatten_zip\s+.*?call \$__sura_array_join_num\s+local\.set \$flatten_zip_label' -or
        $astWatText -notmatch '(?s)local\.get \$flatten_method_source\s+call \$__sura_array_flatten1\s+local\.set \$flatten_method_values' -or
        $astWatText -notmatch '(?s)local\.get \$flatten_method_values\s+.*?call \$__sura_array_join_num\s+local\.set \$flatten_method_label' -or
        $astWatText -notmatch '\(func \$__sura_dict_keys' -or
        $astWatText -notmatch '\(func \$__sura_dict_values' -or
        $astWatText -notmatch '\(func \$__sura_dict_items' -or
        $astWatText -notmatch '\(func \$__sura_dict_pick' -or
        $astWatText -notmatch '\(func \$__sura_dict_omit' -or
        $astWatText -notmatch '\(func \$__sura_dict_merge2' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_source\s+call \$__sura_dict_keys\s+local\.set \$dict_api_keys' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_source\s+call \$__sura_dict_values\s+local\.set \$dict_api_values' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_source\s+call \$__sura_dict_items\s+local\.set \$dict_api_items' -or
        $astWatText -notmatch '(?s)call \$__sura_dict_keys\s+local\.set \$dict_api_direct_keys' -or
        $astWatText -notmatch '(?s)call \$__sura_dict_values\s+local\.set \$dict_api_direct_values' -or
        $astWatText -notmatch '(?s)call \$__sura_dict_items\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$dict_api_direct_item' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_pick_source\s+.*?call \$__sura_dict_pick\s+local\.set \$dict_api_picked' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_pick_source\s+.*?call \$__sura_dict_pick\s+local\.set \$dict_api_picked_method' -or
        $astWatText -notmatch '(?s)call \$__sura_dict_pick\s+local\.set \$dict_api_direct_pick' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_omit_source\s+.*?call \$__sura_dict_omit\s+local\.set \$dict_api_omitted' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_omit_source\s+.*?call \$__sura_dict_omit\s+local\.set \$dict_api_omitted_method' -or
        $astWatText -notmatch '(?s)call \$__sura_dict_omit\s+local\.set \$dict_api_direct_omit' -or
        $astWatText -notmatch '(?s)i32\.const 0\s+local\.get \$dict_api_merge_left\s+call \$__sura_dict_merge2\s+.*?call \$__sura_dict_merge2\s+.*?call \$__sura_dict_merge2\s+local\.set \$dict_api_merged' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_merge_left\s+.*?call \$__sura_dict_merge2\s+local\.set \$dict_api_merged_method' -or
        $astWatText -notmatch '(?s)call \$__sura_dict_merge2\s+.*?call \$__sura_dict_merge2\s+local\.set \$dict_api_direct_merge' -or
        $astWatText -notmatch '(?s)local\.get \$dict_path_source\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+local\.set \$dict_path_hp' -or
        $astWatText -notmatch '(?s)local\.get \$dict_path_source\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_string_or_nil\s+local\.set \$dict_path_name' -or
        $astWatText -notmatch '(?s)local\.get \$dict_path_source\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+local\.set \$json_path_hp' -or
        $astWatText -notmatch '(?s)local\.get \$dict_path_source\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_string_or_nil\s+local\.set \$json_path_name' -or
        $astWatText -notmatch '(?s)local\.get \$dict_path_source\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_has\s+local\.set \$json_has_hp' -or
        $astWatText -notmatch '(?s)local\.get \$dict_path_source\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const \d+\s+call \$__sura_dict_has\s+local\.set \$json_has_missing' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_items\s+i32\.const 0\s+call \$__sura_array_get_checked\s+local\.set \$dict_api_item_first' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_item_first\s+i32\.const 1\s+call \$__sura_array_get_checked\s+local\.set \$dict_api_item_first_value' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_keys\s+.*?call \$__sura_array_join_string\s+local\.set \$dict_api_keys_label' -or
        $astWatText -notmatch '(?s)local\.get \$dict_api_values\s+.*?call \$__sura_array_join_num\s+local\.set \$dict_api_values_label' -or
        $astWatText -notmatch '\(func \$__sura_array_clear' -or
        $astWatText -notmatch '(?s)local\.get \$clear_values\s+call \$__sura_array_clear\s+local\.set \$clear_result' -or
        $astWatText -notmatch '(?s)local\.get \$clear_values\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$clear_len' -or
        $astWatText -notmatch '\(func \$__sura_array_insert' -or
        $astWatText -notmatch '\(func \$__sura_array_remove' -or
        $astWatText -notmatch '(?s)local\.get \$insert_values\s+i32\.const 1\s+i32\.const 2\s+call \$__sura_array_insert\s+local\.set \$insert_values' -or
        $astWatText -notmatch '(?s)local\.get \$insert_values\s+.*?call \$__sura_array_join_num\s+local\.set \$insert_label' -or
        $astWatText -notmatch '(?s)local\.get \$insert_front\s+(?:i32\.const -9|i32\.const 0\s+i32\.const 9\s+i32\.sub)\s+i32\.const 1\s+call \$__sura_array_insert\s+local\.set \$insert_front' -or
        $astWatText -notmatch '(?s)local\.get \$insert_front\s+.*?call \$__sura_array_join_num\s+local\.set \$insert_front_label' -or
        $astWatText -notmatch '(?s)local\.get \$insert_tail\s+i32\.const 99\s+i32\.const 3\s+call \$__sura_array_insert\s+local\.set \$insert_tail' -or
        $astWatText -notmatch '(?s)local\.get \$insert_tail\s+.*?call \$__sura_array_join_num\s+local\.set \$insert_tail_label' -or
        $astWatText -notmatch '(?s)local\.get \$remove_values\s+i32\.const 1\s+i32\.const 1\s+call \$__sura_array_remove\s+local\.set \$removed_mid' -or
        $astWatText -notmatch '(?s)local\.get \$remove_values\s+.*?call \$__sura_array_join_num\s+local\.set \$remove_after_mid_label' -or
        $astWatText -notmatch '(?s)local\.get \$remove_values\s+(?:i32\.const -1|i32\.const 0\s+i32\.const 1\s+i32\.sub)\s+i32\.const 1\s+call \$__sura_array_remove\s+local\.set \$removed_tail' -or
        $astWatText -notmatch '(?s)local\.get \$remove_values\s+.*?call \$__sura_array_join_num\s+local\.set \$remove_after_tail_label' -or
        $astWatText -notmatch '(?s)local\.get \$remove_values\s+i32\.const 9\s+i32\.const 1\s+call \$__sura_array_remove\s+local\.set \$removed_missing' -or
        $astWatText -notmatch '(?s)local\.get \$remove_values\s+i32\.const 4\s+i32\.sub\s+i32\.load\s+local\.set \$remove_missing_len' -or
        $astWatText -notmatch '\(func \$__sura_array_join_string' -or
        $astWatText -notmatch '\(func \$__sura_array_join_num' -or
        $astWatText -notmatch '\(func \$__sura_array_join_bool' -or
        $astWatText -notmatch '\(func \$__sura_array_join_nil' -or
        $astWatText -notmatch '(?s)local\.get \$join_names\s+.*?call \$__sura_array_join_string\s+local\.set \$join_names_label' -or
        $astWatText -notmatch '(?s)local\.get \$join_scores\s+.*?call \$__sura_array_join_num\s+local\.set \$join_scores_label' -or
        $astWatText -notmatch '(?s)local\.get \$join_flags\s+.*?call \$__sura_array_join_bool\s+local\.set \$join_flags_label' -or
        $astWatText -notmatch '(?s)local\.get \$join_nils\s+.*?call \$__sura_array_join_nil\s+local\.set \$join_nils_label' -or
        $astWatText -notmatch '\(func \$__sura_array_slice' -or
        $astWatText -notmatch '(?s)local\.get \$join_scores\s+i32\.const 1\s+i32\.const 2147483647\s+call \$__sura_array_slice\s+local\.set \$slice_scores' -or
        $astWatText -notmatch '(?s)local\.get \$join_names\s+i32\.const 0\s+i32\.const 1\s+call \$__sura_array_slice\s+local\.set \$slice_names' -or
        $astWatText -notmatch '(?s)local\.get \$join_flags\s+(?:i32\.const -2|i32\.const 0\s+i32\.const 2\s+i32\.sub)\s+i32\.const 1\s+call \$__sura_array_slice\s+local\.set \$slice_flags' -or
        $astWatText -notmatch '(?s)local\.get \$join_nils\s+i32\.const 1\s+i32\.const 5\s+call \$__sura_array_slice\s+local\.set \$slice_nils' -or
        $astWatText -notmatch '(?s)local\.get \$slice_scores\s+.*?call \$__sura_array_join_num\s+local\.set \$slice_scores_label' -or
        $astWatText -notmatch '(?s)local\.get \$slice_names\s+.*?call \$__sura_array_join_string\s+local\.set \$slice_names_label' -or
        $astWatText -notmatch '(?s)local\.get \$slice_flags\s+.*?call \$__sura_array_join_bool\s+local\.set \$slice_flags_label' -or
        $astWatText -notmatch '(?s)local\.get \$slice_nils\s+.*?call \$__sura_array_join_nil\s+local\.set \$slice_nils_label' -or
        $astWatText -notmatch '(?s)local\.get \$bools\s+(?:local\.get \$dynamic_idx|i32\.const 1)\s+call \$__sura_array_get_checked.*?call \$__sura_string_concat.*?local\.set \$dynamic_bool_label' -or
        $astWatText -notmatch '(?s)local\.get \$profile\s+.*?call \$__sura_string_hash\s+call \$__sura_dict_has\s+local\.set \$profile_has_name' -or
        $astWatText -notmatch '(?s)local\.get \$profile\s+.*?call \$__sura_string_hash\s+call \$__sura_dict_has\s+local\.set \$profile_has_missing' -or
        $astWatText -notmatch '(?s)local\.get \$profile\s+.*?call \$__sura_string_hash\s+call \$__sura_dict_has\s+local\.set \$profile_contains_city' -or
        $astWatText -notmatch '(?s)local\.get \$profile\s+.*?call \$__sura_string_hash\s+call \$__sura_dict_has\s+local\.set \$dict_module_has_name' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_has_profile\s+.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_receiver_has\s+local\.set \$dynamic_profile_has_missing' -or
        $astWatText -notmatch '(?s)local\.get \$dynamic_has_profile\s+.*?call \$__sura_value_string_or_nil\s+call \$__sura_value_receiver_contains\s+local\.set \$dynamic_profile_contains_name' -or
        $astWatText -notmatch '(?s)local\.get \$profile_has_name.*?local\.get \$dynamic_profile_contains_name.*?local\.set \$dict_has_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\).*?call \$__sura_make_array_4.*?local\.set \$mixed_array_index_to_str_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_type_idx|i32\.const 2)\s+call \$__sura_array_get_checked\s+(?:call \$__sura_value_bool\s+call \$__sura_value_type_name|drop).*?local\.get \$mixed_values\s+(?:local\.get \$mixed_bool_idx|i32\.const 2)\s+call \$__sura_array_get_checked\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy.*?local\.get \$mixed_values\s+(?:local\.get \$mixed_len_idx|i32\.const 1)\s+call \$__sura_array_get_checked\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_length.*?local\.set \$mixed_array_index_runtime_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_profile\s+local\.get \$mixed_profile_active_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_type_name.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_active_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_name_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_length.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_to_string.*?call \$__sura_value_nil\s+call \$__sura_value_to_string.*?local\.set \$mixed_dict_index_runtime_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_bool_idx|i32\.const 2)\s+call \$__sura_array_get_checked\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy\s+i32\.eqz.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_active_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy\s+i32\.eqz.*?call \$__sura_value_nil\s+call \$__sura_value_is_truthy\s+i32\.eqz.*?local\.set \$mixed_exact_not_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_bool_idx|i32\.const 2)\s+call \$__sura_array_get_checked\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy\s+if \(result i32\).*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_active_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy\s+if \(result i32\).*?call \$__sura_value_nil\s+call \$__sura_value_is_truthy\s+if \(result i32\).*?call \$__sura_value_nil\s+call \$__sura_value_is_truthy\s+if \(result i32\).*?local\.set \$mixed_exact_logic_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_bool_idx|i32\.const 2)\s+call \$__sura_array_get_checked\s+call \$__sura_value_bool\s+i32\.const 1\s+call \$__sura_value_bool\s+call \$__sura_value_eq.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_active_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+i32\.const 1\s+call \$__sura_value_bool\s+call \$__sura_value_eq.*?call \$__sura_value_nil\s+call \$__sura_value_nil\s+call \$__sura_value_eq.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+i32\.const 17\s+call \$__sura_value_num\s+call \$__sura_value_eq\s+i32\.eqz.*?local\.set \$mixed_exact_eq_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const 16\s+i32\.ge_s.*?local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const 20\s+i32\.lt_s.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 10\s+i32\.gt_s.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 16\s+i32\.le_s.*?local\.set \$mixed_exact_compare_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const 4\s+i32\.add.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 6\s+i32\.sub.*?local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const 2\s+i32\.mul.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+i32\.const 4\s+call \$__sura_value_num\s+call \$__sura_value_div.*?call \$__sura_value_to_string.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 5\s+call \$__sura_i32_mod.*?local\.set \$mixed_exact_arith_label' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const 7\s+i32\.and.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 3\s+i32\.or.*?local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const 3\s+i32\.xor.*?local\.get \$mixed_profile\s+local\.get \$mixed_profile_score_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+i32\.const 1\s+call \$__sura_value_num\s+i32\.const 3\s+call \$__sura_value_bitwise.*?local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+call \$__sura_value_num\s+i32\.const 1\s+call \$__sura_value_num\s+i32\.const 4\s+call \$__sura_value_bitwise.*?local\.get \$mixed_values\s+(?:local\.get \$mixed_score_idx|i32\.const 0)\s+call \$__sura_array_get_checked\s+i32\.const -1\s+i32\.xor.*?local\.set \$mixed_exact_bitwise_label' -or
        $astWatText -notmatch '(?s)local\.get \$profile\s+local\.get \$profile_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get.*?call \$__sura_string_concat.*?local\.set \$profile_dynamic_label' -or
        $astWatText -notmatch '(?s)local\.get \$profile\s+local\.get \$profile_update_key.*?call \$__sura_dict_put\s+local\.set \$profile' -or
        $astWatText -notmatch '(?s)local\.get \$profile.*?call \$__sura_value_dict.*?call \$__sura_value_type_name.*?local\.get \$names.*?call \$__sura_value_array.*?call \$__sura_value_type_name.*?local\.get \$flags\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_type_name.*?call \$__sura_value_nil\s+call \$__sura_value_type_name.*?local\.get \$score\s+call \$__sura_value_num\s+call \$__sura_value_type_name.*?local\.get \$point\s+drop.*?i32\.const 105\s+.*?local\.set \$type_label' -or
        $astWatText -notmatch '(?s)local\.get \$profile.*?call \$__sura_value_dict.*?call \$__sura_value_length.*?call \$__sura_i32_to_string.*?local\.get \$names.*?call \$__sura_value_array.*?call \$__sura_value_length.*?call \$__sura_i32_to_string.*?local\.get \$profile.*?call \$__sura_value_dict.*?call \$__sura_value_type_name\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_length.*?local\.set \$len_label' -or
        $astWatText -notmatch '(?s)local\.get \$score\s+call \$__sura_i32_to_string.*?local\.set \$str_label' -or
        $astWatText -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_to_string.*?local\.set \$literal_array_to_str_label' -or
        $astWatText -notmatch '(?s)call \$__sura_value_dynamic_dict\s+call \$__sura_value_to_string.*?local\.set \$literal_dict_to_str_label' -or
        $astWatText -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_to_string.*?call \$__sura_value_dynamic_dict\s+call \$__sura_value_to_string.*?local\.set \$runtime_collection_to_str_label' -or
        $astWatText -notmatch '(?s)local\.get \$profile.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy.*?local\.set \$bool_label' -or
        $astWatText -match 'call \$(length|len|assert_len|to_str)' -or
        $astWatText -notmatch 'local\.set \$ast_string_search_label' -or
        $astWatText -notmatch 'call \$__sura_string_contains' -or
        $astWatText -notmatch 'call \$__sura_string_starts_with' -or
        $astWatText -notmatch 'call \$__sura_string_ends_with' -or
        $astWatText -notmatch 'local\.set \$ast_string_index_of_label' -or
        $astWatText -notmatch 'call \$__sura_string_index_of' -or
        $astWatText -notmatch 'local\.set \$ast_string_transform_label' -or
        $astWatText -notmatch 'call \$__sura_string_upper' -or
        $astWatText -notmatch 'call \$__sura_string_lower' -or
        $astWatText -notmatch 'call \$__sura_string_trim' -or
        $astWatText -notmatch 'local\.set \$ast_string_replace_label' -or
        $astWatText -notmatch 'call \$__sura_string_replace' -or
        $astWatText -notmatch 'local\.set \$ast_string_slice_label' -or
        $astWatText -notmatch 'call \$__sura_string_substring' -or
        $astWatText -notmatch 'call \$__sura_string_slice' -or
        $astWatText -notmatch 'local\.set \$ast_string_repeat_label' -or
        $astWatText -notmatch '\(func \$__sura_string_repeat' -or
        $astWatText -notmatch '(?s)i32\.const 3\s+call \$__sura_string_repeat' -or
        $astWatText -notmatch '(?s)i32\.const 2\s+call \$__sura_string_repeat' -or
        $astWatText -notmatch '(?s)i32\.const 4\s+call \$__sura_string_repeat' -or
        $astWatText -notmatch 'local\.set \$ast_string_pad_label' -or
        $astWatText -notmatch '\(func \$__sura_string_pad_left' -or
        $astWatText -notmatch '\(func \$__sura_string_pad_right' -or
        $astWatText -notmatch 'call \$__sura_string_pad_left' -or
        $astWatText -notmatch 'call \$__sura_string_pad_right' -or
        $astWatText -notmatch 'local\.set \$split_parts' -or
        $astWatText -notmatch 'local\.set \$split_module_parts' -or
        $astWatText -notmatch 'local\.set \$split_empty_parts' -or
        $astWatText -notmatch 'local\.set \$ast_string_split_label' -or
        $astWatText -notmatch '\(func \$__sura_string_split' -or
        $astWatText -notmatch '(?s)call \$__sura_string_split\s+local\.set \$split_parts' -or
        $astWatText -notmatch '(?s)call \$__sura_string_split\s+local\.set \$split_module_parts' -or
        $astWatText -notmatch '(?s)local\.get \$split_parts\s+i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $astWatText -notmatch '(?s)local\.get \$split_parts\s+.*?call \$__sura_array_join_string\s+.*?local\.get \$split_module_parts' -or
        $astWatText -notmatch 'local\.set \$line_parts' -or
        $astWatText -notmatch 'local\.set \$line_module_parts' -or
        $astWatText -notmatch 'local\.set \$line_receiver_parts' -or
        $astWatText -notmatch 'local\.set \$empty_line_parts' -or
        $astWatText -notmatch 'local\.set \$ast_string_lines_label' -or
        $astWatText -notmatch '\(func \$__sura_string_lines' -or
        $astWatText -notmatch 'call \$__sura_string_lines' -or
        $astWatText -notmatch '(?s)local\.get \$line_parts\s+i32\.const 2\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $astWatText -notmatch '(?s)local\.get \$line_receiver_parts\s+.*?call \$__sura_array_join_string\s+.*?local\.get \$empty_line_parts' -or
        $astWatText -notmatch 'local\.set \$word_parts' -or
        $astWatText -notmatch 'local\.set \$word_module_parts' -or
        $astWatText -notmatch 'local\.set \$word_receiver_parts' -or
        $astWatText -notmatch 'local\.set \$empty_word_parts' -or
        $astWatText -notmatch 'local\.set \$ast_string_words_label' -or
        $astWatText -notmatch '\(func \$__sura_string_words' -or
        $astWatText -notmatch 'call \$__sura_string_words' -or
        $astWatText -notmatch '(?s)local\.get \$word_parts\s+i32\.const 2\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $astWatText -notmatch '(?s)local\.get \$word_receiver_parts\s+.*?call \$__sura_array_join_string\s+.*?local\.get \$empty_word_parts' -or
        $astWatText -notmatch 'local\.set \$chunk_parts' -or
        $astWatText -notmatch 'local\.set \$chunk_alias_parts' -or
        $astWatText -notmatch 'local\.set \$chunk_module_parts' -or
        $astWatText -notmatch 'local\.set \$chunk_receiver_parts' -or
        $astWatText -notmatch 'local\.set \$empty_chunk_parts' -or
        $astWatText -notmatch 'local\.set \$ast_text_chunks_label' -or
        $astWatText -notmatch '\(func \$__sura_text_chunks' -or
        $astWatText -notmatch 'call \$__sura_text_chunks' -or
        $astWatText -notmatch '(?s)i32\.const 3\s+i32\.const 1\s+call \$__sura_text_chunks\s+local\.set \$chunk_parts' -or
        $astWatText -notmatch '(?s)local\.get \$chunk_parts\s+i32\.const 2\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $astWatText -notmatch '(?s)local\.get \$chunk_module_parts\s+.*?call \$__sura_array_join_string\s+.*?local\.get \$chunk_receiver_parts' -or
        $astWatText -notmatch 'local\.set \$compare_label' -or
        $astWatText -notmatch 'local\.set \$tag_label' -or
        $astWatText -notmatch 'local\.set \$object_index_label' -or
        $astWatText -notmatch 'local\.set \$object_dot_label' -or
        $astWatText -notmatch 'local\.set \$object_key_label' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_index_label' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_key_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_tagged_map' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_assigned_key_label' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_assigned_field_label' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_assigned_index_label' -or
        $astWatText -notmatch 'local\.set \$mixed_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_dynamic_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_string_label' -or
        $astWatText -notmatch 'local\.set \$mixed_bool_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_bool_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_bool_label' -or
        $astWatText -notmatch 'local\.set \$mixed_nil_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_nil_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_nil_label' -or
        $astWatText -notmatch 'local\.set \$mixed_object_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_object_dict' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_exact_dict_label' -or
        $astWatText -notmatch 'local\.set \$mixed_array_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_array_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_array_label' -or
        $astWatText -notmatch 'local\.set \$mixed_nested_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_nested_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_dict_label' -or
        $astWatText -notmatch 'local\.set \$mixed_function_key_lookup' -or
        $astWatText -notmatch 'local\.set \$mixed_function_dict' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_function_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_dict_assigned_function_same' -or
        $astWatText -notmatch 'local\.set \$dynamic_tagged_items' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_assigned_array_label' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_assigned_array_field_label' -or
        $astWatText -notmatch 'local\.set \$object_dynamic_assigned_array_index_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_string_items' -or
        $astWatText -notmatch 'local\.set \$dynamic_assigned_string_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_bool_items' -or
        $astWatText -notmatch 'local\.set \$dynamic_assigned_bool_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_nil_items' -or
        $astWatText -notmatch 'local\.set \$dynamic_assigned_nil_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_number_items' -or
        $astWatText -notmatch 'local\.set \$dynamic_assigned_num_label' -or
        $astWatText -notmatch 'local\.set \$dynamic_assigned_num_calc' -or
        $astWatText -notmatch 'local\.set \$param_tag_label' -or
        $astWatText -notmatch 'local\.set \$direct_interp_dot_label' -or
        $astWatText -notmatch 'local\.set \$direct_interp_method_label' -or
        $astWatText -notmatch 'local\.set \$direct_interp_ternary_label' -or
        $astWatText -notmatch 'local\.set \$direct_interp_index_label' -or
        $astWatText -notmatch 'local\.set \$direct_interp_ternary_index_label' -or
        $astWatText -notmatch 'local\.set \$pi_label' -or
        $astWatText -notmatch 'local\.set \$score_concat_label' -or
        $astWatText -notmatch 'local\.set \$mutable_label' -or
        $astWatText -notmatch 'local\.set \$compound_numbers' -or
        $astWatText -notmatch 'local\.set \$compound_label' -or
        $astWatText -notmatch 'local\.set \$compound_names' -or
        $astWatText -notmatch 'local\.set \$compound_profile' -or
        $astWatText -notmatch 'local\.set \$compound_string_label' -or
        $astWatText -notmatch 'local\.set \$field_values' -or
        $astWatText -notmatch 'local\.set \$field_assign_label' -or
        $astWatText -notmatch 'local\.set \$score_suffix' -or
        $astWatText -notmatch 'local\.set \$compound_op_score' -or
        $astWatText -notmatch 'local\.set \$compound_ops_label' -or
        $astWatText -notmatch 'local\.set \$name_join' -or
        $astWatText -notmatch 'local\.set \$indexed_name_join' -or
        $astWatText -notmatch 'local\.set \$inline_name_join' -or
        $astWatText -notmatch 'local\.set \$name_index_sum' -or
        $astWatText -notmatch 'local\.set \$profile_key_join' -or
        $astWatText -notmatch 'local\.set \$profile_value_join' -or
        $astWatText -notmatch 'local\.set \$meta_key_join' -or
        $astWatText -notmatch 'local\.set \$meta_value_sum' -or
        $astWatText -notmatch 'local\.set \$dict_single_count' -or
        $astWatText -notmatch 'local\.set \$profile_key' -or
        $astWatText -notmatch 'local\.set \$profile_value' -or
        $astWatText -notmatch 'local\.set \$meta_key' -or
        $astWatText -notmatch 'local\.set \$meta_value' -or
        $astWatText -notmatch 'local\.set \$__result' -or
        $astWatText -notmatch 'console output API as main-result text' -or
        $astWatText -match 'local\.get \$console' -or
        $astWatText -match 'call \$console' -or
        $astWatText -notmatch 'call \$__sura_i32_to_string' -or
        $astWatText -notmatch 'call \$__sura_string_concat' -or
        $astWatText -notmatch 'i32\.const 7' -or
        $astWatText -notmatch 'i32\.const 56' -or
        $astWatText -notmatch '\(local \$__ast_for_step' -or
        $astWatText -notmatch 'call \$__sura_alloc' -or
        $astWatText -match 'i32\.const 4call' -or
        $astWatText -match 'local\.get \$field_conflict_(num|text)call') {
        throw "generated AST JSON WASM should lower enum constants, arrays, imports, calls, numeric string interpolation, console output, range-for, and print result directly from AST nodes"
    }
    if ($astWatText -notmatch '\(global \$__sura_object_registry \(mut i32\) \(i32.const 0\)\)' -or
        $astWatText -notmatch '\(func \$__sura_object_register' -or
        $astWatText -notmatch '\(func \$__sura_object_class_id' -or
        $astWatText -notmatch '(?s)\(func \$__sura_new_AstDynamicBase.*?call \$__sura_object_register' -or
        $astWatText -notmatch '(?s)\(func \$__sura_new_AstDynamicScale.*?call \$__sura_object_register' -or
        $astWatText -notmatch '(?s)\(func \$__sura_new_AstDynamicInherited.*?call \$__sura_object_register' -or
        $astWatText -notmatch '(?s)\(func \$dynamic_method_score_ast.*?call \$__sura_call_method_\d+_1' -or
        $astWatText -notmatch '(?s)\(func \$dynamic_inherited_score_ast.*?call \$__sura_call_method_\d+_1' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_method_\d+_1.*?call \$__sura_method_AstDynamicBase_score.*?call \$__sura_method_AstDynamicScale_score') {
        throw "generated AST JSON WASM should register class identity out-of-band and dynamically dispatch overridden and inherited methods"
    }
    if ($astWatText -notmatch '\(func \$__sura_value_index' -or
        ([regex]::Matches($astWatText, 'call \$__sura_value_index').Count -lt 4)) {
        throw "generated AST JSON WASM should lower unknown mixed collection indexes through the Value index helper"
    }
    if ($astWatText -notmatch '\(func \$__sura_value_field' -or
        ([regex]::Matches($astWatText, 'call \$__sura_value_field').Count -lt 2)) {
        throw "generated AST JSON WASM should lower mixed receiver dot access through the Value field helper"
    }
    if ($astWatText -notmatch '(?s)\(func \$captured_inline_function_param_ast.*?i32\.const 5\s+local\.get \$bonus\s+i32\.add\s+return' -or
        $astWatText -match '(?s)\(func \$__sura_func_expr_[^\s)]*.*?local\.get \$bonus') {
        throw "runtime-param-capturing inline function expressions should inline inside the owner function without emitting invalid lifted closures"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 7\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_captured_inline_function') {
        throw "returned inline function expressions that capture call arguments should specialize to a dispatchable function value"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_function_capture_[^\s)]*.*?local\.get \$value\s+(call \$block_double_ast|i32\.const 2\s+i32\.mul).*?return' -or
        $astWatText -notmatch '(?s)i32\.const [0-9]+\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_function_capture' -or
        $astWatText -match '(?s)\(func \$__sura_func_expr_returned_param_function_capture_[^\s)]*.*?call \$handler') {
        throw "returned inline function expressions should specialize captured function-valued call arguments into callable lifted targets"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_alias_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_alias_captured_inline_function') {
        throw "returned inline function expressions should specialize pure local aliases derived from call arguments"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_if_alias_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 1\s+i32\.const 7\s+drop\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_if_alias_captured_inline_function') {
        throw "returned inline function expressions should merge pure if-branch captures before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_if_false_no_else_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 0\s+i32\.const 7\s+drop\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_if_false_no_else_captured_inline_function') {
        throw "returned inline function expressions should preserve captures when static false if has no else"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_if_unknown_no_else_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_if_unknown_no_else_captured_inline_function') {
        throw "returned inline function expressions should merge unknown if-without-else captures with the empty else path"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_match_alias_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 1\s+i32\.const 7\s+drop\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_match_alias_captured_inline_function') {
        throw "returned inline function expressions should merge pure match-arm captures before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_match_no_arm_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 2\s+i32\.const 7\s+drop\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_match_no_arm_captured_inline_function') {
        throw "returned inline function expressions should preserve captures when static match executes no arms"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_repeat_alias_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_repeat_alias_captured_inline_function') {
        throw "returned inline function expressions should apply exact repeat-one captures before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_repeat_accum_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 13\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_repeat_accum_captured_inline_function') {
        throw "returned inline function expressions should apply bounded repeat capture updates before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_for_accum_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 19\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_for_accum_captured_inline_function') {
        throw "returned inline function expressions should apply bounded static for-loop capture updates before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_for_zero_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_for_zero_captured_inline_function') {
        throw "returned inline function expressions should skip zero-iteration static for loops before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_foreach_accum_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 19\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_foreach_accum_captured_inline_function') {
        throw "returned inline function expressions should apply static foreach capture updates before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_foreach_empty_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_foreach_empty_captured_inline_function') {
        throw "returned inline function expressions should skip empty static foreach loops before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_foreach_empty_dict_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_foreach_empty_dict_captured_inline_function') {
        throw "returned inline function expressions should skip empty static dict foreach loops before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_while_false_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 0\s+i32\.const 7\s+drop\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_while_false_captured_inline_function') {
        throw "returned inline function expressions should skip captured false while loops before specialization"
    }
    if ($astWatText -notmatch '(?s)\(func \$__sura_func_expr_returned_param_while_literal_false_captured_inline_function_[^\s)]*.*?local\.get \$value\s+i32\.const 8\s+i32\.add\s+return' -or
        $astWatText -notmatch '(?s)i32\.const 7\s+drop\s+i32\.const [0-9]+\s+local\.set \$returned_param_while_literal_false_captured_inline_function') {
        throw "returned inline function expressions should skip literal false while loops before specialization"
    }
    $valueRuntimeAccessInterpIndex = $astWatText.IndexOf('local.set $value_runtime_access_interp_label')
    if ($valueRuntimeAccessInterpIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime access interpolation"
    }
    $valueRuntimeAccessInterpStart = [Math]::Max(0, $valueRuntimeAccessInterpIndex - 12000)
    $valueRuntimeAccessInterpWindow = $astWatText.Substring($valueRuntimeAccessInterpStart, $valueRuntimeAccessInterpIndex - $valueRuntimeAccessInterpStart)
    if ($valueRuntimeAccessInterpWindow -notmatch 'call \$__sura_value_index' -or
        $valueRuntimeAccessInterpWindow -notmatch 'call \$__sura_value_field' -or
        ([regex]::Matches($valueRuntimeAccessInterpWindow, 'call \$__sura_value_to_string').Count -lt 3)) {
        throw "mixed runtime index/dot interpolation should stringify through the tagged Value runtime"
    }
    $valueRuntimeAccessCallIndex = $astWatText.IndexOf('local.set $value_runtime_access_call_label')
    if ($valueRuntimeAccessCallIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime access call lowering"
    }
    $valueRuntimeAccessCallStart = [Math]::Max(0, $valueRuntimeAccessCallIndex - 36000)
    $valueRuntimeAccessCallWindow = $astWatText.Substring($valueRuntimeAccessCallStart, $valueRuntimeAccessCallIndex - $valueRuntimeAccessCallStart)
    if (([regex]::Matches($valueRuntimeAccessCallWindow, 'call \$__sura_value_index').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAccessCallWindow, 'call \$__sura_value_field').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeAccessCallWindow, 'call \$__sura_value_length').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAccessCallWindow, 'call \$__sura_value_is_truthy').Count -lt 3)) {
        throw "mixed runtime index/dot length() and to_bool() should lower through the tagged Value runtime"
    }
    $valueRuntimeAccessEqIndex = $astWatText.IndexOf('local.set $value_runtime_access_eq_label')
    if ($valueRuntimeAccessEqIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime access equality lowering"
    }
    $valueRuntimeAccessEqStart = [Math]::Max(0, $valueRuntimeAccessEqIndex - 36000)
    $valueRuntimeAccessEqWindow = $astWatText.Substring($valueRuntimeAccessEqStart, $valueRuntimeAccessEqIndex - $valueRuntimeAccessEqStart)
    if (([regex]::Matches($valueRuntimeAccessEqWindow, 'call \$__sura_value_index').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAccessEqWindow, 'call \$__sura_value_field').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAccessEqWindow, 'call \$__sura_value_eq').Count -lt 4)) {
        throw "mixed runtime index/dot equality should lower through the tagged Value equality runtime"
    }
    $valueRuntimeAccessNumericIndex = $astWatText.IndexOf('local.set $value_runtime_access_numeric_label')
    if ($valueRuntimeAccessNumericIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime access numeric lowering"
    }
    $valueRuntimeAccessNumericStart = [Math]::Max(0, $valueRuntimeAccessNumericIndex - 36000)
    $valueRuntimeAccessNumericWindow = $astWatText.Substring($valueRuntimeAccessNumericStart, $valueRuntimeAccessNumericIndex - $valueRuntimeAccessNumericStart)
    if (([regex]::Matches($valueRuntimeAccessNumericWindow, 'call \$__sura_value_index').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeAccessNumericWindow, 'call \$__sura_value_field').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeAccessNumericWindow, 'call \$__sura_value_add').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeAccessNumericWindow, 'call \$__sura_value_mul').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeAccessNumericWindow, 'call \$__sura_value_mod').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeAccessNumericWindow, 'call \$__sura_value_numeric_compare').Count -lt 1)) {
        throw "mixed runtime index/dot numeric arithmetic and comparison should preserve tagged numeric Values"
    }
    $valueRuntimeAccessNumericTernaryIndex = $astWatText.IndexOf('local.set $value_runtime_access_numeric_ternary_label')
    if ($valueRuntimeAccessNumericTernaryIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime access numeric ternary lowering"
    }
    $valueRuntimeAccessNumericTernaryStart = [Math]::Max(0, $valueRuntimeAccessNumericTernaryIndex - 42000)
    $valueRuntimeAccessNumericTernaryWindow = $astWatText.Substring($valueRuntimeAccessNumericTernaryStart, $valueRuntimeAccessNumericTernaryIndex - $valueRuntimeAccessNumericTernaryStart)
    if ($valueRuntimeAccessNumericTernaryWindow -notmatch 'if \(result i32\)' -or
        $valueRuntimeAccessNumericTernaryWindow -notmatch 'call \$__sura_value_index' -or
        $valueRuntimeAccessNumericTernaryWindow -notmatch 'call \$__sura_value_field' -or
        ([regex]::Matches($valueRuntimeAccessNumericTernaryWindow, 'call \$__sura_value_add').Count -lt 2)) {
        throw "mixed runtime index/dot numeric ternary branches should preserve tagged numeric Values"
    }
    $valueRuntimeAccessNumericCoalesceIndex = $astWatText.IndexOf('local.set $value_runtime_access_numeric_coalesce_label')
    if ($valueRuntimeAccessNumericCoalesceIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime access numeric null coalescing"
    }
    $valueRuntimeAccessNumericCoalesceStart = [Math]::Max(0, $valueRuntimeAccessNumericCoalesceIndex - 52000)
    $valueRuntimeAccessNumericCoalesceWindow = $astWatText.Substring($valueRuntimeAccessNumericCoalesceStart, $valueRuntimeAccessNumericCoalesceIndex - $valueRuntimeAccessNumericCoalesceStart)
    if (([regex]::Matches($valueRuntimeAccessNumericCoalesceWindow, 'call \$__sura_value_tag').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeAccessNumericCoalesceWindow, 'call \$__sura_value_add').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeAccessNumericCoalesceWindow, 'call \$__sura_value_index').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAccessNumericCoalesceWindow, 'call \$__sura_value_field').Count -lt 2)) {
        throw "mixed runtime index/dot numeric null coalescing should use tagged Value nil checks and tagged arithmetic"
    }
    $valueRuntimeCoalesceReceiverIndex = $astWatText.IndexOf('local.set $value_runtime_coalesce_receiver_label')
    if ($valueRuntimeCoalesceReceiverIndex -lt 0) {
        throw "generated AST JSON WASM should include Value coalesced receiver access lowering"
    }
    $valueRuntimeCoalesceReceiverStart = [Math]::Max(0, $valueRuntimeCoalesceReceiverIndex - 72000)
    $valueRuntimeCoalesceReceiverWindow = $astWatText.Substring($valueRuntimeCoalesceReceiverStart, $valueRuntimeCoalesceReceiverIndex - $valueRuntimeCoalesceReceiverStart)
    if (([regex]::Matches($valueRuntimeCoalesceReceiverWindow, 'call \$__sura_value_tag').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeCoalesceReceiverWindow, 'call \$__sura_value_field').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeCoalesceReceiverWindow, 'call \$__sura_value_index').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeCoalesceReceiverWindow, 'call \$__sura_value_to_string').Count -lt 3)) {
        throw "coalesced mixed collection receivers should lower field/index access through tagged Value runtime"
    }
    $valueRuntimeNestedAccessIndex = $astWatText.IndexOf('local.set $value_runtime_nested_access_label')
    if ($valueRuntimeNestedAccessIndex -lt 0) {
        throw "generated AST JSON WASM should include nested Value runtime access lowering"
    }
    $valueRuntimeNestedAccessStart = [Math]::Max(0, $valueRuntimeNestedAccessIndex - 90000)
    $valueRuntimeNestedAccessWindow = $astWatText.Substring($valueRuntimeNestedAccessStart, $valueRuntimeNestedAccessIndex - $valueRuntimeNestedAccessStart)
    if (([regex]::Matches($valueRuntimeNestedAccessWindow, 'call \$__sura_value_field').Count -lt 8) -or
        ([regex]::Matches($valueRuntimeNestedAccessWindow, 'call \$__sura_value_index').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeNestedAccessWindow, 'call \$__sura_value_to_string').Count -lt 3) -or
        $valueRuntimeNestedAccessWindow -notmatch 'call \$__sura_value_type_name') {
        throw "nested mixed runtime dot/index chains should continue through tagged Value field and index helpers"
    }
    $valueRuntimeSameDictShapeIndex = $astWatText.IndexOf('local.set $value_runtime_same_dict_shape_label')
    if ($valueRuntimeSameDictShapeIndex -lt 0) {
        throw "generated AST JSON WASM should include same-dict-shape Value runtime access lowering"
    }
    $valueRuntimeSameDictShapeStart = [Math]::Max(0, $valueRuntimeSameDictShapeIndex - 70000)
    $valueRuntimeSameDictShapeWindow = $astWatText.Substring($valueRuntimeSameDictShapeStart, $valueRuntimeSameDictShapeIndex - $valueRuntimeSameDictShapeStart)
    if (([regex]::Matches($valueRuntimeSameDictShapeWindow, 'call \$__sura_value_dynamic_dict').Count -lt 6) -or
        ([regex]::Matches($valueRuntimeSameDictShapeWindow, 'call \$__sura_value_field').Count -lt 6) -or
        ([regex]::Matches($valueRuntimeSameDictShapeWindow, 'call \$__sura_value_type_name').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeSameDictShapeWindow, 'call \$__sura_value_to_string').Count -lt 3)) {
        throw "same-type dict ternary receivers with divergent key/value shapes should lower field reads through tagged Value helpers"
    }
    $valueRuntimeSameArrayShapeIndex = $astWatText.IndexOf('local.set $value_runtime_same_array_shape_label')
    if ($valueRuntimeSameArrayShapeIndex -lt 0) {
        throw "generated AST JSON WASM should include same-array-shape Value runtime index lowering"
    }
    $valueRuntimeSameArrayShapeStart = [Math]::Max(0, $valueRuntimeSameArrayShapeIndex - 78000)
    $valueRuntimeSameArrayShapeWindow = $astWatText.Substring($valueRuntimeSameArrayShapeStart, $valueRuntimeSameArrayShapeIndex - $valueRuntimeSameArrayShapeStart)
    if (([regex]::Matches($valueRuntimeSameArrayShapeWindow, 'call \$__sura_value_dynamic_array').Count -lt 6) -or
        ([regex]::Matches($valueRuntimeSameArrayShapeWindow, 'call \$__sura_value_index').Count -lt 6) -or
        ([regex]::Matches($valueRuntimeSameArrayShapeWindow, 'call \$__sura_value_type_name').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeSameArrayShapeWindow, 'call \$__sura_value_to_string').Count -lt 3)) {
        throw "same-type array ternary receivers with divergent element shapes should lower index reads through tagged Value helpers"
    }
    $valueRuntimeCoalesceShapeIndex = $astWatText.IndexOf('local.set $value_runtime_coalesce_shape_label')
    if ($valueRuntimeCoalesceShapeIndex -lt 0) {
        throw "generated AST JSON WASM should include coalesced same-shape Value runtime access lowering"
    }
    $valueRuntimeCoalesceShapeStart = [Math]::Max(0, $valueRuntimeCoalesceShapeIndex - 92000)
    $valueRuntimeCoalesceShapeWindow = $astWatText.Substring($valueRuntimeCoalesceShapeStart, $valueRuntimeCoalesceShapeIndex - $valueRuntimeCoalesceShapeStart)
    if (([regex]::Matches($valueRuntimeCoalesceShapeWindow, 'call \$__sura_value_tag').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeCoalesceShapeWindow, 'call \$__sura_value_field').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeCoalesceShapeWindow, 'call \$__sura_value_index').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeCoalesceShapeWindow, 'call \$__sura_value_type_name').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeCoalesceShapeWindow, 'call \$__sura_value_to_string').Count -lt 4)) {
        throw "same-type coalesced dict/array receivers with divergent shapes should lower field/index reads through tagged Value helpers"
    }
    $valueRuntimeStringMethodIndex = $astWatText.IndexOf('local.set $value_runtime_string_method_label')
    if ($valueRuntimeStringMethodIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime string method lowering"
    }
    $valueRuntimeStringMethodStart = [Math]::Max(0, $valueRuntimeStringMethodIndex - 80000)
    $valueRuntimeStringMethodWindow = $astWatText.Substring($valueRuntimeStringMethodStart, $valueRuntimeStringMethodIndex - $valueRuntimeStringMethodStart)
    if (([regex]::Matches($valueRuntimeStringMethodWindow, 'call \$__sura_value_to_string').Count -lt 4) -or
        $valueRuntimeStringMethodWindow -notmatch 'call \$__sura_string_trim' -or
        $valueRuntimeStringMethodWindow -notmatch 'call \$__sura_string_upper' -or
        $valueRuntimeStringMethodWindow -notmatch 'call \$__sura_value_receiver_contains' -or
        $valueRuntimeStringMethodWindow -notmatch 'call \$__sura_value_receiver_index_of') {
        throw "string receiver methods on mixed runtime Value accesses should stringify through Value helpers before string helper calls"
    }
    $valueRuntimeStringChainIndex = $astWatText.IndexOf('local.set $value_runtime_string_chain_label')
    if ($valueRuntimeStringChainIndex -lt 0) {
        throw "generated AST JSON WASM should include chained Value runtime string method lowering"
    }
    $valueRuntimeStringChainStart = [Math]::Max(0, $valueRuntimeStringChainIndex - 90000)
    $valueRuntimeStringChainWindow = $astWatText.Substring($valueRuntimeStringChainStart, $valueRuntimeStringChainIndex - $valueRuntimeStringChainStart)
    if (([regex]::Matches($valueRuntimeStringChainWindow, 'call \$__sura_value_to_string').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeStringChainWindow, 'call \$__sura_string_replace').Count -lt 2) -or
        $valueRuntimeStringChainWindow -notmatch 'call \$__sura_string_upper' -or
        $valueRuntimeStringChainWindow -notmatch 'call \$__sura_string_substring' -or
        $valueRuntimeStringChainWindow -notmatch 'call \$__sura_string_lower' -or
        $valueRuntimeStringChainWindow -notmatch 'call \$__sura_string_slice' -or
        $valueRuntimeStringChainWindow -notmatch 'call \$__sura_string_contains' -or
        $valueRuntimeStringChainWindow -notmatch 'call \$__sura_string_index_of') {
        throw "chained string receiver methods on mixed runtime Value accesses should preserve string method type hints"
    }
    $valueRuntimeStringCallIndex = $astWatText.IndexOf('local.set $value_runtime_string_call_label')
    if ($valueRuntimeStringCallIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime string call lowering"
    }
    $valueRuntimeStringCallStart = [Math]::Max(0, $valueRuntimeStringCallIndex - 90000)
    $valueRuntimeStringCallWindow = $astWatText.Substring($valueRuntimeStringCallStart, $valueRuntimeStringCallIndex - $valueRuntimeStringCallStart)
    if (([regex]::Matches($valueRuntimeStringCallWindow, 'call \$__sura_value_to_string').Count -lt 7) -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_upper' -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_lower' -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_trim' -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_replace' -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_contains' -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_index_of' -or
        $valueRuntimeStringCallWindow -notmatch 'call \$__sura_string_substring') {
        throw "global and module string calls on mixed runtime Value accesses should stringify through Value helpers"
    }
    $valueRuntimeStringSearchAliasVariantIndex = $astWatText.IndexOf('local.set $value_runtime_string_search_alias_variant_label')
    if ($valueRuntimeStringSearchAliasVariantIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime string search alias variant lowering"
    }
    $valueRuntimeStringSearchAliasVariantStart = [Math]::Max(0, $valueRuntimeStringSearchAliasVariantIndex - 50000)
    $valueRuntimeStringSearchAliasVariantWindow = $astWatText.Substring($valueRuntimeStringSearchAliasVariantStart, $valueRuntimeStringSearchAliasVariantIndex - $valueRuntimeStringSearchAliasVariantStart)
    if ($valueRuntimeStringSearchAliasVariantWindow -match 'call \$string_indexOf' -or
        $valueRuntimeStringSearchAliasVariantWindow -match 'call \$string_startsWith' -or
        $valueRuntimeStringSearchAliasVariantWindow -match 'call \$string_endsWith' -or
        $valueRuntimeStringSearchAliasVariantWindow -notmatch 'call \$__sura_string_index_of' -or
        $valueRuntimeStringSearchAliasVariantWindow -notmatch 'call \$__sura_string_starts_with' -or
        $valueRuntimeStringSearchAliasVariantWindow -notmatch 'call \$__sura_string_ends_with') {
        throw "string camelCase direct aliases on mixed runtime accesses should lower to string search helpers"
    }
    $valueRuntimeLenMethodIndex = $astWatText.IndexOf('local.set $value_runtime_len_method_label')
    if ($valueRuntimeLenMethodIndex -lt 0) {
        throw "generated AST JSON WASM should include Value runtime len() method lowering"
    }
    $valueRuntimeLenMethodStart = [Math]::Max(0, $valueRuntimeLenMethodIndex - 70000)
    $valueRuntimeLenMethodWindow = $astWatText.Substring($valueRuntimeLenMethodStart, $valueRuntimeLenMethodIndex - $valueRuntimeLenMethodStart)
    if (([regex]::Matches($valueRuntimeLenMethodWindow, 'call \$__sura_value_length').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeLenMethodWindow, 'call \$__sura_value_index').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeLenMethodWindow, 'call \$__sura_value_field').Count -lt 3)) {
        throw "len() receiver methods on mixed runtime Value accesses should use tagged Value length helpers"
    }
    $valueRuntimeAliasIndex = $astWatText.IndexOf('local.set $value_runtime_alias_label')
    if ($valueRuntimeAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned Value runtime alias lowering"
    }
    $valueRuntimeAliasStart = [Math]::Max(0, $valueRuntimeAliasIndex - 90000)
    $valueRuntimeAliasWindow = $astWatText.Substring($valueRuntimeAliasStart, $valueRuntimeAliasIndex - $valueRuntimeAliasStart)
    if (([regex]::Matches($valueRuntimeAliasWindow, 'local\.set \$value_runtime_(?:alias|missing_alias|dot_alias|num_alias)').Count -lt 4) -or
        ([regex]::Matches($valueRuntimeAliasWindow, 'call \$__sura_value_index').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAliasWindow, 'call \$__sura_value_field').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeAliasWindow, 'call \$__sura_value_type_name').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeAliasWindow, 'call \$__sura_value_to_string').Count -lt 4) -or
        $valueRuntimeAliasWindow -notmatch 'call \$__sura_value_is_truthy' -or
        $valueRuntimeAliasWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeAliasWindow -notmatch 'call \$__sura_value_eq' -or
        $valueRuntimeAliasWindow -notmatch 'call \$__sura_value_add') {
        throw "assigned mixed runtime Value aliases should preserve the tagged Value ABI across type/to_str/to_bool/length/equality/string/numeric use"
    }
    $valueRuntimeAliasMethodIndex = $astWatText.IndexOf('local.set $value_runtime_alias_method_label')
    if ($valueRuntimeAliasMethodIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned Value runtime alias string method lowering"
    }
    $valueRuntimeAliasMethodStart = [Math]::Max(0, $valueRuntimeAliasMethodIndex - 70000)
    $valueRuntimeAliasMethodWindow = $astWatText.Substring($valueRuntimeAliasMethodStart, $valueRuntimeAliasMethodIndex - $valueRuntimeAliasMethodStart)
    if (([regex]::Matches($valueRuntimeAliasMethodWindow, 'call \$__sura_value_to_string').Count -lt 4) -or
        $valueRuntimeAliasMethodWindow -notmatch 'call \$__sura_string_upper' -or
        $valueRuntimeAliasMethodWindow -notmatch 'call \$__sura_string_starts_with' -or
        $valueRuntimeAliasMethodWindow -notmatch 'call \$__sura_string_replace' -or
        $valueRuntimeAliasMethodWindow -notmatch 'call \$__sura_string_lower' -or
        $valueRuntimeAliasMethodWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeAliasMethodWindow -notmatch 'call \$__sura_string_contains') {
        throw "string methods and module calls on assigned mixed runtime Value aliases should stringify through Value helpers"
    }
    $valueRuntimeCollectionAliasIndex = $astWatText.IndexOf('local.set $value_runtime_collection_alias_label')
    if ($valueRuntimeCollectionAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed collection Value alias lowering"
    }
    $valueRuntimeCollectionAliasStart = [Math]::Max(0, $valueRuntimeCollectionAliasIndex - 90000)
    $valueRuntimeCollectionAliasWindow = $astWatText.Substring($valueRuntimeCollectionAliasStart, $valueRuntimeCollectionAliasIndex - $valueRuntimeCollectionAliasStart)
    if (([regex]::Matches($valueRuntimeCollectionAliasWindow, 'local\.set \$value_runtime_(?:collection_alias|profile_alias)').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeCollectionAliasWindow, 'call \$__sura_value_dynamic_array').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeCollectionAliasWindow, 'call \$__sura_value_dynamic_dict').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeCollectionAliasWindow, 'call \$__sura_value_index').Count -lt 3) -or
        ([regex]::Matches($valueRuntimeCollectionAliasWindow, 'call \$__sura_value_field').Count -lt 4) -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_value_type_name' -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_value_add' -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_string_upper' -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_string_contains' -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_value_receiver_contains' -or
        $valueRuntimeCollectionAliasWindow -notmatch 'call \$__sura_value_receiver_index_of') {
        throw "assigned mixed collection Value aliases should support later index/dot/string/numeric/array-method lowering through Value helpers"
    }
    $valueRuntimeCollectionLenMethodIndex = $astWatText.IndexOf('local.set $value_runtime_collection_len_method_label')
    if ($valueRuntimeCollectionLenMethodIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed collection Value receiver length method lowering"
    }
    $valueRuntimeCollectionLenMethodStart = [Math]::Max(0, $valueRuntimeCollectionLenMethodIndex - 25000)
    $valueRuntimeCollectionLenMethodWindow = $astWatText.Substring($valueRuntimeCollectionLenMethodStart, $valueRuntimeCollectionLenMethodIndex - $valueRuntimeCollectionLenMethodStart)
    if ($valueRuntimeCollectionLenMethodWindow -notmatch 'call \$__sura_value_length') {
        throw "assigned mixed collection Value aliases should support len()/length() receiver methods through Value length"
    }
    $valueRuntimeNumberCollectionAliasIndex = $astWatText.IndexOf('local.set $value_runtime_number_collection_alias_label')
    if ($valueRuntimeNumberCollectionAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed numeric collection Value alias lowering"
    }
    $valueRuntimeNumberCollectionAliasStart = [Math]::Max(0, $valueRuntimeNumberCollectionAliasIndex - 50000)
    $valueRuntimeNumberCollectionAliasWindow = $astWatText.Substring($valueRuntimeNumberCollectionAliasStart, $valueRuntimeNumberCollectionAliasIndex - $valueRuntimeNumberCollectionAliasStart)
    if ($valueRuntimeNumberCollectionAliasWindow -notmatch 'call \$__sura_value_array_sum' -or
        $valueRuntimeNumberCollectionAliasWindow -notmatch 'call \$__sura_value_array_avg' -or
        $valueRuntimeNumberCollectionAliasWindow -notmatch 'call \$__sura_value_array_min' -or
        $valueRuntimeNumberCollectionAliasWindow -notmatch 'call \$__sura_value_array_max') {
        throw "assigned mixed numeric collection Value aliases should support aggregate receiver methods through Value array helpers"
    }
    $valueRuntimeMutableIndex = $astWatText.IndexOf('local.set $value_runtime_mutable_label')
    if ($valueRuntimeMutableIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed collection Value mutation lowering"
    }
    $valueRuntimeMutableStart = [Math]::Max(0, $valueRuntimeMutableIndex - 22000)
    $valueRuntimeMutableWindow = $astWatText.Substring($valueRuntimeMutableStart, $valueRuntimeMutableIndex - $valueRuntimeMutableStart)
    if ($valueRuntimeMutableWindow -notmatch 'call \$__sura_value_set_index' -or
        $valueRuntimeMutableWindow -notmatch 'call \$__sura_value_set_field' -or
        $valueRuntimeMutableWindow -notmatch 'call \$__sura_value_index' -or
        $valueRuntimeMutableWindow -notmatch 'call \$__sura_value_field' -or
        $valueRuntimeMutableWindow -notmatch 'call \$__sura_value_to_string') {
        throw "assigned mixed collection Value aliases should support index and dot mutation through tagged Value setters"
    }
    $valueRuntimeArrayModuleAliasIndex = $astWatText.IndexOf('local.set $value_runtime_array_module_alias_label')
    if ($valueRuntimeArrayModuleAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed collection Value array module lowering"
    }
    $valueRuntimeArrayModuleAliasStart = [Math]::Max(0, $valueRuntimeArrayModuleAliasIndex - 65000)
    $valueRuntimeArrayModuleAliasWindow = $astWatText.Substring($valueRuntimeArrayModuleAliasStart, $valueRuntimeArrayModuleAliasIndex - $valueRuntimeArrayModuleAliasStart)
    if ($valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_array_sum' -or
        $valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_array_avg' -or
        $valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_array_min' -or
        $valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_array_max' -or
        $valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_array_contains' -or
        $valueRuntimeArrayModuleAliasWindow -notmatch 'call \$__sura_value_array_index_of') {
        throw "assigned mixed collection Value aliases should support array module calls through Value array helpers"
    }
    $valueRuntimeArrayDirectAliasIndex = $astWatText.IndexOf('local.set $value_runtime_array_direct_alias_label')
    if ($valueRuntimeArrayDirectAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed collection Value array direct alias lowering"
    }
    $valueRuntimeArrayDirectAliasStart = [Math]::Max(0, $valueRuntimeArrayDirectAliasIndex - 65000)
    $valueRuntimeArrayDirectAliasWindow = $astWatText.Substring($valueRuntimeArrayDirectAliasStart, $valueRuntimeArrayDirectAliasIndex - $valueRuntimeArrayDirectAliasStart)
    if ($valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_array_sum' -or
        $valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_array_avg' -or
        $valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_array_min' -or
        $valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_array_max' -or
        $valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_array_contains' -or
        $valueRuntimeArrayDirectAliasWindow -notmatch 'call \$__sura_value_array_index_of') {
        throw "assigned mixed collection Value aliases should support array direct alias calls through Value array helpers"
    }
    $valueRuntimeArrayAliasVariantIndex = $astWatText.IndexOf('local.set $value_runtime_array_alias_variant_label')
    if ($valueRuntimeArrayAliasVariantIndex -lt 0) {
        throw "generated AST JSON WASM should include assigned mixed collection Value array alias variant lowering"
    }
    $valueRuntimeArrayAliasVariantStart = [Math]::Max(0, $valueRuntimeArrayAliasVariantIndex - 65000)
    $valueRuntimeArrayAliasVariantWindow = $astWatText.Substring($valueRuntimeArrayAliasVariantStart, $valueRuntimeArrayAliasVariantIndex - $valueRuntimeArrayAliasVariantStart)
    if ($valueRuntimeArrayAliasVariantWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeArrayAliasVariantWindow -notmatch 'call \$__sura_value_array_avg' -or
        $valueRuntimeArrayAliasVariantWindow -notmatch 'call \$__sura_value_array_contains' -or
        $valueRuntimeArrayAliasVariantWindow -notmatch 'call \$__sura_value_array_index_of') {
        throw "assigned mixed collection Value aliases should support array alias variants through Value array helpers"
    }
    $valueRuntimeReturnAliasIndex = $astWatText.IndexOf('local.set $value_runtime_return_alias_label')
    if ($valueRuntimeReturnAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include function-returned mixed collection Value alias lowering"
    }
    $valueRuntimeReturnAliasStart = [Math]::Max(0, $valueRuntimeReturnAliasIndex - 110000)
    $valueRuntimeReturnAliasWindow = $astWatText.Substring($valueRuntimeReturnAliasStart, $valueRuntimeReturnAliasIndex - $valueRuntimeReturnAliasStart)
    if ($valueRuntimeReturnAliasWindow -notmatch 'call \$make_value_collection_alias_ast' -or
        $valueRuntimeReturnAliasWindow -notmatch 'call \$make_value_profile_alias_ast' -or
        ([regex]::Matches($valueRuntimeReturnAliasWindow, 'local\.set \$value_runtime_return_(?:collection_alias|profile_alias)').Count -lt 2) -or
        ([regex]::Matches($valueRuntimeReturnAliasWindow, 'call \$__sura_value_dynamic_array').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeReturnAliasWindow, 'call \$__sura_value_dynamic_dict').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeReturnAliasWindow, 'call \$__sura_value_index').Count -lt 1) -or
        ([regex]::Matches($valueRuntimeReturnAliasWindow, 'call \$__sura_value_field').Count -lt 4) -or
        $valueRuntimeReturnAliasWindow -notmatch 'call \$__sura_value_length' -or
        $valueRuntimeReturnAliasWindow -notmatch 'call \$__sura_value_type_name' -or
        $valueRuntimeReturnAliasWindow -notmatch 'call \$__sura_value_add') {
        throw "function-returned mixed collection Values should support later index/dot/string/numeric lowering through Value helpers"
    }
    $valueRuntimeReturnMutableIndex = $astWatText.IndexOf('local.set $value_runtime_return_mutable_label')
    if ($valueRuntimeReturnMutableIndex -lt 0) {
        throw "generated AST JSON WASM should include function-returned mixed collection Value mutation lowering"
    }
    $valueRuntimeReturnMutableStart = [Math]::Max(0, $valueRuntimeReturnMutableIndex - 26000)
    $valueRuntimeReturnMutableWindow = $astWatText.Substring($valueRuntimeReturnMutableStart, $valueRuntimeReturnMutableIndex - $valueRuntimeReturnMutableStart)
    if ($valueRuntimeReturnMutableWindow -notmatch 'call \$__sura_value_set_index' -or
        $valueRuntimeReturnMutableWindow -notmatch 'call \$__sura_value_set_field' -or
        $valueRuntimeReturnMutableWindow -notmatch 'call \$__sura_value_index' -or
        $valueRuntimeReturnMutableWindow -notmatch 'call \$__sura_value_field' -or
        $astWatText -notmatch '(?s)\(func \$__sura_value_set_index.*?local\.get \$tag\s+i32\.const 8\s+i32\.eq\s+if.*?call \$__sura_array_set_checked\s+return\s+end\s+local\.get \$tag\s+i32\.const 4\s+i32\.eq\s+if.*?call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)\(func \$__sura_value_set_index.*?i32\.const 9\s+i32\.eq\s+local\.get \$tag\s+i32\.const 5\s+i32\.eq\s+i32\.or' -or
        $astWatText -notmatch '(?s)\(func \$__sura_value_field.*?i32\.const 9\s+i32\.eq\s+local\.get \$receiver\s+call \$__sura_value_tag\s+i32\.const 5\s+i32\.eq\s+i32\.or' -or
        $astWatText -notmatch '(?s)\(func \$__sura_value_set_field.*?i32\.const 9\s+i32\.eq\s+local\.get \$receiver\s+call \$__sura_value_tag\s+i32\.const 5\s+i32\.eq\s+i32\.or') {
        throw "function-returned mixed collection Value aliases should support raw and dynamic Value array/dict reads and mutation through tagged helpers"
    }
    $valueRuntimeMethodMutableIndex = $astWatText.IndexOf('local.set $value_runtime_method_mutable_label')
    if ($valueRuntimeMethodMutableIndex -lt 0) {
        throw "generated AST JSON WASM should include method-returned mixed collection Value mutation lowering"
    }
    $valueRuntimeMethodMutableStart = [Math]::Max(0, $valueRuntimeMethodMutableIndex - 32000)
    $valueRuntimeMethodMutableWindow = $astWatText.Substring($valueRuntimeMethodMutableStart, $valueRuntimeMethodMutableIndex - $valueRuntimeMethodMutableStart)
    if ($valueRuntimeMethodMutableWindow -notmatch 'call \$__sura_method_AstCollectionTagged_pick_mixed_value_items' -or
        $valueRuntimeMethodMutableWindow -notmatch 'call \$__sura_method_AstCollectionTagged_pick_mixed_value_profile' -or
        $valueRuntimeMethodMutableWindow -notmatch 'call \$__sura_value_set_index' -or
        $valueRuntimeMethodMutableWindow -notmatch 'call \$__sura_value_set_field' -or
        $valueRuntimeMethodMutableWindow -notmatch 'call \$__sura_value_index' -or
        $valueRuntimeMethodMutableWindow -notmatch 'call \$__sura_value_field' -or
        $valueRuntimeMethodMutableWindow -match '(?s)local\.get \$value_runtime_method_collection_alias\s+i32\.const 1\s+i32\.const 4\s+i32\.mul') {
        throw "method-returned mixed collection Value aliases should keep tagged Value ABI for later mutation and reads"
    }
    $valueRuntimeSuperMutableIndex = $astWatText.IndexOf('local.set $value_runtime_super_mutable_label')
    if ($valueRuntimeSuperMutableIndex -lt 0) {
        throw "generated AST JSON WASM should include super-returned mixed collection Value mutation lowering"
    }
    $valueRuntimeSuperMutableStart = [Math]::Max(0, $valueRuntimeSuperMutableIndex - 36000)
    $valueRuntimeSuperMutableWindow = $astWatText.Substring($valueRuntimeSuperMutableStart, $valueRuntimeSuperMutableIndex - $valueRuntimeSuperMutableStart)
    if ($valueRuntimeSuperMutableWindow -notmatch 'call \$__sura_method_AstCollectionChild_mixed_items_via_super' -or
        $valueRuntimeSuperMutableWindow -notmatch 'call \$__sura_method_AstCollectionChild_mixed_profile_via_super' -or
        $valueRuntimeSuperMutableWindow -notmatch 'call \$__sura_value_set_index' -or
        $valueRuntimeSuperMutableWindow -notmatch 'call \$__sura_value_set_field' -or
        $valueRuntimeSuperMutableWindow -notmatch 'call \$__sura_value_index' -or
        $valueRuntimeSuperMutableWindow -notmatch 'call \$__sura_value_field' -or
        $valueRuntimeSuperMutableWindow -match '(?s)local\.get \$value_runtime_super_collection_alias\s+i32\.const 1\s+i32\.const 4\s+i32\.mul') {
        throw "super-returned mixed collection Value aliases should keep tagged Value ABI for later mutation and reads"
    }
    foreach ($caughtFunctionCallName in @("try_function_value_call", "try_function_param_payload_call", "try_method_function_value_call")) {
        $caughtFunctionCallIndex = $astWatText.LastIndexOf("local.set `$$caughtFunctionCallName")
        if ($caughtFunctionCallIndex -lt 0) {
            throw "generated AST JSON WASM should include $caughtFunctionCallName"
        }
        $caughtFunctionCallStart = [Math]::Max(0, $caughtFunctionCallIndex - 520)
        $caughtFunctionCallWindow = $astWatText.Substring($caughtFunctionCallStart, $caughtFunctionCallIndex - $caughtFunctionCallStart)
        if ($caughtFunctionCallWindow -notmatch '(call \$block_double_ast|i32\.const 2\s+i32\.mul)' -or $caughtFunctionCallWindow -match 'call \$err') {
            throw "caught function-valued throw payloads should lower through the recovered lifted WASM function target or its single-return inline body, not the catch variable"
        }
    }
    $functionParamCallStart = $astWatText.IndexOf('(func $call_function_param_ast')
    if ($functionParamCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_function_param_ast"
    }
    $functionParamCallNext = $astWatText.IndexOf("  (func ", $functionParamCallStart + 1)
    if ($functionParamCallNext -lt 0) { $functionParamCallNext = $astWatText.Length }
    $functionParamCallBody = $astWatText.Substring($functionParamCallStart, $functionParamCallNext - $functionParamCallStart)
    if ($functionParamCallBody -notmatch '(call \$block_double_ast|i32\.const 2\s+i32\.mul)' -or $functionParamCallBody -match 'call \$handler') {
        throw "function-valued parameters with a single observed lifted target should lower through the lifted WASM function or its single-return inline body, not the parameter name"
    }
    $dynamicFunctionParamCallStart = $astWatText.IndexOf('(func $call_dynamic_function_param_ast')
    if ($dynamicFunctionParamCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_function_param_ast"
    }
    $dynamicFunctionParamCallNext = $astWatText.IndexOf("  (func ", $dynamicFunctionParamCallStart + 1)
    if ($dynamicFunctionParamCallNext -lt 0) { $dynamicFunctionParamCallNext = $astWatText.Length }
    $dynamicFunctionParamCallBody = $astWatText.Substring($dynamicFunctionParamCallStart, $dynamicFunctionParamCallNext - $dynamicFunctionParamCallStart)
    if ($dynamicFunctionParamCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicFunctionParamCallBody -match 'call \$handler') {
        throw "function-valued parameters with multiple observed lifted targets should dispatch through the WASM function-value dispatcher"
    }
    $dynamicLocalFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_local_function_ast')
    if ($dynamicLocalFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_local_function_ast"
    }
    $dynamicLocalFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicLocalFunctionCallStart + 1)
    if ($dynamicLocalFunctionCallNext -lt 0) { $dynamicLocalFunctionCallNext = $astWatText.Length }
    $dynamicLocalFunctionCallBody = $astWatText.Substring($dynamicLocalFunctionCallStart, $dynamicLocalFunctionCallNext - $dynamicLocalFunctionCallStart)
    if ($dynamicLocalFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicLocalFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases selected by runtime ternaries should dispatch through the WASM function-value dispatcher"
    }
    $dynamicZeroFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_zero_function_ast')
    if ($dynamicZeroFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_zero_function_ast"
    }
    $dynamicZeroFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicZeroFunctionCallStart + 1)
    if ($dynamicZeroFunctionCallNext -lt 0) { $dynamicZeroFunctionCallNext = $astWatText.Length }
    $dynamicZeroFunctionCallBody = $astWatText.Substring($dynamicZeroFunctionCallStart, $dynamicZeroFunctionCallNext - $dynamicZeroFunctionCallStart)
    if ($dynamicZeroFunctionCallBody -notmatch 'call \$__sura_call_function_0' -or $dynamicZeroFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases with zero arguments should dispatch through the WASM function-value dispatcher"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_0 \(param \$id i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_0.*?call \$zero_a_ast.*?call \$zero_b_ast') {
        throw "WASM function-value dispatcher should include same-arity zero-argument function targets"
    }
    $dynamicZeroFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_zero_function_call_label')
    if ($dynamicZeroFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_zero_function_call_label"
    }
    $dynamicZeroFunctionLabelStart = [Math]::Max(0, $dynamicZeroFunctionLabelIndex - 700)
    $dynamicZeroFunctionLabelWindow = $astWatText.Substring($dynamicZeroFunctionLabelStart, $dynamicZeroFunctionLabelIndex - $dynamicZeroFunctionLabelStart)
    if ($dynamicZeroFunctionLabelWindow -notmatch 'call \$call_dynamic_zero_function_ast' -or
        $dynamicZeroFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic zero-argument function-value dispatch results should preserve the user function calls"
    }
    $dynamicBinaryFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_binary_function_ast')
    if ($dynamicBinaryFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_binary_function_ast"
    }
    $dynamicBinaryFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicBinaryFunctionCallStart + 1)
    if ($dynamicBinaryFunctionCallNext -lt 0) { $dynamicBinaryFunctionCallNext = $astWatText.Length }
    $dynamicBinaryFunctionCallBody = $astWatText.Substring($dynamicBinaryFunctionCallStart, $dynamicBinaryFunctionCallNext - $dynamicBinaryFunctionCallStart)
    if ($dynamicBinaryFunctionCallBody -notmatch 'call \$__sura_call_function_2' -or $dynamicBinaryFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases with two arguments should dispatch through the WASM function-value dispatcher"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_2 \(param \$id i32\) \(param \$arg0 i32\) \(param \$arg1 i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_2.*?call \$add_pair_ast.*?call \$mul_pair_ast') {
        throw "WASM function-value dispatcher should include same-arity two-argument function targets"
    }
    $dynamicBinaryFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_binary_function_call_label')
    if ($dynamicBinaryFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_binary_function_call_label"
    }
    $dynamicBinaryFunctionLabelStart = [Math]::Max(0, $dynamicBinaryFunctionLabelIndex - 800)
    $dynamicBinaryFunctionLabelWindow = $astWatText.Substring($dynamicBinaryFunctionLabelStart, $dynamicBinaryFunctionLabelIndex - $dynamicBinaryFunctionLabelStart)
    if ($dynamicBinaryFunctionLabelWindow -notmatch 'call \$call_dynamic_binary_function_ast' -or
        $dynamicBinaryFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic two-argument function-value dispatch results should preserve the user function calls"
    }
    $dynamicTripleFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_triple_function_ast')
    if ($dynamicTripleFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_triple_function_ast"
    }
    $dynamicTripleFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicTripleFunctionCallStart + 1)
    if ($dynamicTripleFunctionCallNext -lt 0) { $dynamicTripleFunctionCallNext = $astWatText.Length }
    $dynamicTripleFunctionCallBody = $astWatText.Substring($dynamicTripleFunctionCallStart, $dynamicTripleFunctionCallNext - $dynamicTripleFunctionCallStart)
    if ($dynamicTripleFunctionCallBody -notmatch 'call \$__sura_call_function_3' -or $dynamicTripleFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases with three arguments should dispatch through the WASM function-value dispatcher"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_3 \(param \$id i32\) \(param \$arg0 i32\) \(param \$arg1 i32\) \(param \$arg2 i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_3.*?call \$mul_add_triple_ast.*?call \$sum_triple_ast') {
        throw "WASM function-value dispatcher should include same-arity three-argument function targets"
    }
    $dynamicTripleFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_triple_function_call_label')
    if ($dynamicTripleFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_triple_function_call_label"
    }
    $dynamicTripleFunctionLabelStart = [Math]::Max(0, $dynamicTripleFunctionLabelIndex - 900)
    $dynamicTripleFunctionLabelWindow = $astWatText.Substring($dynamicTripleFunctionLabelStart, $dynamicTripleFunctionLabelIndex - $dynamicTripleFunctionLabelStart)
    if ($dynamicTripleFunctionLabelWindow -notmatch 'call \$call_dynamic_triple_function_ast' -or
        $dynamicTripleFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic three-argument function-value dispatch results should preserve the user function calls"
    }
    $dynamicQuadFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_quad_function_ast')
    if ($dynamicQuadFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_quad_function_ast"
    }
    $dynamicQuadFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicQuadFunctionCallStart + 1)
    if ($dynamicQuadFunctionCallNext -lt 0) { $dynamicQuadFunctionCallNext = $astWatText.Length }
    $dynamicQuadFunctionCallBody = $astWatText.Substring($dynamicQuadFunctionCallStart, $dynamicQuadFunctionCallNext - $dynamicQuadFunctionCallStart)
    if ($dynamicQuadFunctionCallBody -notmatch 'call \$__sura_call_function_4' -or $dynamicQuadFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases with four arguments should dispatch through the WASM function-value dispatcher"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_4 \(param \$id i32\) \(param \$arg0 i32\) \(param \$arg1 i32\) \(param \$arg2 i32\) \(param \$arg3 i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_4.*?call \$mul_add_quad_ast.*?call \$sum_quad_ast') {
        throw "WASM function-value dispatcher should include same-arity four-argument function targets"
    }
    $dynamicQuadFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_quad_function_call_label')
    if ($dynamicQuadFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_quad_function_call_label"
    }
    $dynamicQuadFunctionLabelStart = [Math]::Max(0, $dynamicQuadFunctionLabelIndex - 900)
    $dynamicQuadFunctionLabelWindow = $astWatText.Substring($dynamicQuadFunctionLabelStart, $dynamicQuadFunctionLabelIndex - $dynamicQuadFunctionLabelStart)
    if ($dynamicQuadFunctionLabelWindow -notmatch 'call \$call_dynamic_quad_function_ast' -or
        $dynamicQuadFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic four-argument function-value dispatch results should preserve the user function calls"
    }
    $dynamicFiveFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_five_function_ast')
    if ($dynamicFiveFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_five_function_ast"
    }
    $dynamicFiveFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicFiveFunctionCallStart + 1)
    if ($dynamicFiveFunctionCallNext -lt 0) { $dynamicFiveFunctionCallNext = $astWatText.Length }
    $dynamicFiveFunctionCallBody = $astWatText.Substring($dynamicFiveFunctionCallStart, $dynamicFiveFunctionCallNext - $dynamicFiveFunctionCallStart)
    if ($dynamicFiveFunctionCallBody -notmatch 'call \$__sura_call_function_5' -or $dynamicFiveFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases with five arguments should dispatch through the WASM function-value dispatcher"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_5 \(param \$id i32\) \(param \$arg0 i32\) \(param \$arg1 i32\) \(param \$arg2 i32\) \(param \$arg3 i32\) \(param \$arg4 i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_5.*?call \$mul_add_five_ast.*?call \$sum_five_ast') {
        throw "WASM function-value dispatcher should include same-arity five-argument function targets"
    }
    $dynamicFiveFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_five_function_call_label')
    if ($dynamicFiveFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_five_function_call_label"
    }
    $dynamicFiveFunctionLabelStart = [Math]::Max(0, $dynamicFiveFunctionLabelIndex - 900)
    $dynamicFiveFunctionLabelWindow = $astWatText.Substring($dynamicFiveFunctionLabelStart, $dynamicFiveFunctionLabelIndex - $dynamicFiveFunctionLabelStart)
    if ($dynamicFiveFunctionLabelWindow -notmatch 'call \$call_dynamic_five_function_ast' -or
        $dynamicFiveFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic five-argument function-value dispatch results should preserve the user function calls"
    }
    $dynamicEightFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_eight_function_ast')
    if ($dynamicEightFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_eight_function_ast"
    }
    $dynamicEightFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicEightFunctionCallStart + 1)
    if ($dynamicEightFunctionCallNext -lt 0) { $dynamicEightFunctionCallNext = $astWatText.Length }
    $dynamicEightFunctionCallBody = $astWatText.Substring($dynamicEightFunctionCallStart, $dynamicEightFunctionCallNext - $dynamicEightFunctionCallStart)
    if ($dynamicEightFunctionCallBody -notmatch 'call \$__sura_call_function_8' -or $dynamicEightFunctionCallBody -match 'call \$handler') {
        throw "function-valued local aliases with eight arguments should dispatch through the WASM function-value dispatcher"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_8 \(param \$id i32\) \(param \$arg0 i32\) \(param \$arg1 i32\) \(param \$arg2 i32\) \(param \$arg3 i32\) \(param \$arg4 i32\) \(param \$arg5 i32\) \(param \$arg6 i32\) \(param \$arg7 i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_8.*?call \$mul_add_eight_ast.*?call \$sum_eight_ast') {
        throw "WASM function-value dispatcher should include same-arity eight-argument function targets"
    }
    $dynamicEightFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_eight_function_call_label')
    if ($dynamicEightFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_eight_function_call_label"
    }
    $dynamicEightFunctionLabelStart = [Math]::Max(0, $dynamicEightFunctionLabelIndex - 1200)
    $dynamicEightFunctionLabelWindow = $astWatText.Substring($dynamicEightFunctionLabelStart, $dynamicEightFunctionLabelIndex - $dynamicEightFunctionLabelStart)
    if ($dynamicEightFunctionLabelWindow -notmatch 'call \$call_dynamic_eight_function_ast' -or
        $dynamicEightFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic eight-argument function-value dispatch results should preserve the user function calls"
    }
    $dynamicStringFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_string_function_ast')
    if ($dynamicStringFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_string_function_ast"
    }
    $dynamicStringFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicStringFunctionCallStart + 1)
    if ($dynamicStringFunctionCallNext -lt 0) { $dynamicStringFunctionCallNext = $astWatText.Length }
    $dynamicStringFunctionCallBody = $astWatText.Substring($dynamicStringFunctionCallStart, $dynamicStringFunctionCallNext - $dynamicStringFunctionCallStart)
    if ($dynamicStringFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicStringFunctionCallBody -match 'call \$handler') {
        throw "string-returning function-valued local aliases should dispatch through the WASM function-value dispatcher"
    }
    $dynamicStringFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_string_function_call_label')
    if ($dynamicStringFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_string_function_call_label"
    }
    $dynamicStringFunctionLabelStart = [Math]::Max(0, $dynamicStringFunctionLabelIndex - 900)
    $dynamicStringFunctionLabelWindow = $astWatText.Substring($dynamicStringFunctionLabelStart, $dynamicStringFunctionLabelIndex - $dynamicStringFunctionLabelStart)
    if ($dynamicStringFunctionLabelWindow -notmatch 'call \$call_dynamic_string_function_ast' -or
        $dynamicStringFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic string-returning function dispatch results should preserve the user function calls"
    }
    $dynamicBoolFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_bool_function_ast')
    if ($dynamicBoolFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_bool_function_ast"
    }
    $dynamicBoolFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicBoolFunctionCallStart + 1)
    if ($dynamicBoolFunctionCallNext -lt 0) { $dynamicBoolFunctionCallNext = $astWatText.Length }
    $dynamicBoolFunctionCallBody = $astWatText.Substring($dynamicBoolFunctionCallStart, $dynamicBoolFunctionCallNext - $dynamicBoolFunctionCallStart)
    if ($dynamicBoolFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicBoolFunctionCallBody -match 'call \$handler') {
        throw "bool-returning function-valued local aliases should dispatch through the WASM function-value dispatcher"
    }
    $dynamicBoolFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_bool_function_call_label')
    if ($dynamicBoolFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_bool_function_call_label"
    }
    $dynamicBoolFunctionLabelStart = [Math]::Max(0, $dynamicBoolFunctionLabelIndex - 2600)
    $dynamicBoolFunctionLabelWindow = $astWatText.Substring($dynamicBoolFunctionLabelStart, $dynamicBoolFunctionLabelIndex - $dynamicBoolFunctionLabelStart)
    if ($dynamicBoolFunctionLabelWindow -notmatch 'call \$call_dynamic_bool_function_ast' -or
        $dynamicBoolFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic bool-returning function dispatch results should preserve the user function calls"
    }
    $dynamicArrayFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_array_function_ast')
    if ($dynamicArrayFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_array_function_ast"
    }
    $dynamicArrayFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicArrayFunctionCallStart + 1)
    if ($dynamicArrayFunctionCallNext -lt 0) { $dynamicArrayFunctionCallNext = $astWatText.Length }
    $dynamicArrayFunctionCallBody = $astWatText.Substring($dynamicArrayFunctionCallStart, $dynamicArrayFunctionCallNext - $dynamicArrayFunctionCallStart)
    if ($dynamicArrayFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicArrayFunctionCallBody -match 'call \$handler') {
        throw "array-returning function-valued local aliases should dispatch through the WASM function-value dispatcher"
    }
    $dynamicArrayFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_array_function_call_label')
    if ($dynamicArrayFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_array_function_call_label"
    }
    $dynamicArrayFunctionLabelStart = [Math]::Max(0, $dynamicArrayFunctionLabelIndex - 1000)
    $dynamicArrayFunctionLabelWindow = $astWatText.Substring($dynamicArrayFunctionLabelStart, $dynamicArrayFunctionLabelIndex - $dynamicArrayFunctionLabelStart)
    if ($dynamicArrayFunctionLabelWindow -notmatch 'call \$call_dynamic_array_function_ast' -or
        $dynamicArrayFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "dynamic array-returning function dispatch should preserve collection return handles for length/to_str"
    }
    $dynamicNilFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_nil_function_ast')
    if ($dynamicNilFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_nil_function_ast"
    }
    $dynamicNilFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicNilFunctionCallStart + 1)
    if ($dynamicNilFunctionCallNext -lt 0) { $dynamicNilFunctionCallNext = $astWatText.Length }
    $dynamicNilFunctionCallBody = $astWatText.Substring($dynamicNilFunctionCallStart, $dynamicNilFunctionCallNext - $dynamicNilFunctionCallStart)
    if ($dynamicNilFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicNilFunctionCallBody -match 'call \$handler') {
        throw "nil-returning function-valued local aliases should dispatch through the WASM function-value dispatcher"
    }
    $dynamicNilFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_nil_function_call_label')
    if ($dynamicNilFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_nil_function_call_label"
    }
    $dynamicNilFunctionLabelStart = [Math]::Max(0, $dynamicNilFunctionLabelIndex - 2600)
    $dynamicNilFunctionLabelWindow = $astWatText.Substring($dynamicNilFunctionLabelStart, $dynamicNilFunctionLabelIndex - $dynamicNilFunctionLabelStart)
    if ($dynamicNilFunctionLabelWindow -notmatch 'call \$call_dynamic_nil_function_ast' -or
        $dynamicNilFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "dynamic nil-returning function dispatch should preserve nil type/truthiness at the caller"
    }
    $dynamicDictReturnFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_dict_return_function_ast')
    if ($dynamicDictReturnFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_dict_return_function_ast"
    }
    $dynamicDictReturnFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicDictReturnFunctionCallStart + 1)
    if ($dynamicDictReturnFunctionCallNext -lt 0) { $dynamicDictReturnFunctionCallNext = $astWatText.Length }
    $dynamicDictReturnFunctionCallBody = $astWatText.Substring($dynamicDictReturnFunctionCallStart, $dynamicDictReturnFunctionCallNext - $dynamicDictReturnFunctionCallStart)
    if ($dynamicDictReturnFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicDictReturnFunctionCallBody -match 'call \$handler') {
        throw "dict-returning function-valued local aliases should dispatch through the WASM function-value dispatcher"
    }
    $dynamicDictReturnFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_dict_return_function_call_label')
    if ($dynamicDictReturnFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_dict_return_function_call_label"
    }
    $dynamicDictReturnFunctionLabelStart = [Math]::Max(0, $dynamicDictReturnFunctionLabelIndex - 2600)
    $dynamicDictReturnFunctionLabelWindow = $astWatText.Substring($dynamicDictReturnFunctionLabelStart, $dynamicDictReturnFunctionLabelIndex - $dynamicDictReturnFunctionLabelStart)
    if ($dynamicDictReturnFunctionLabelWindow -notmatch 'call \$call_dynamic_dict_return_function_ast' -or
        $dynamicDictReturnFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "dynamic dict-returning function dispatch should preserve dict handles for length/field/to_str"
    }
    $dynamicFunctionReturnFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_function_return_function_ast')
    if ($dynamicFunctionReturnFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_function_return_function_ast"
    }
    $dynamicFunctionReturnFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicFunctionReturnFunctionCallStart + 1)
    if ($dynamicFunctionReturnFunctionCallNext -lt 0) { $dynamicFunctionReturnFunctionCallNext = $astWatText.Length }
    $dynamicFunctionReturnFunctionCallBody = $astWatText.Substring($dynamicFunctionReturnFunctionCallStart, $dynamicFunctionReturnFunctionCallNext - $dynamicFunctionReturnFunctionCallStart)
    if ($dynamicFunctionReturnFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicFunctionReturnFunctionCallBody -match 'call \$handler') {
        throw "function-returning function-valued local aliases should dispatch through the WASM function-value dispatcher"
    }
    $dynamicFunctionReturnFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_function_return_function_call_label')
    if ($dynamicFunctionReturnFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_function_return_function_call_label"
    }
    $dynamicFunctionReturnFunctionLabelStart = [Math]::Max(0, $dynamicFunctionReturnFunctionLabelIndex - 2200)
    $dynamicFunctionReturnFunctionLabelWindow = $astWatText.Substring($dynamicFunctionReturnFunctionLabelStart, $dynamicFunctionReturnFunctionLabelIndex - $dynamicFunctionReturnFunctionLabelStart)
    if ($dynamicFunctionReturnFunctionLabelWindow -notmatch 'call \$__sura_call_function_2' -or
        $dynamicFunctionReturnFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "dynamic function-returning function dispatch should preserve the returned function value for later invocation"
    }
    $dynamicDictLookupDictReturnFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_dict_lookup_dict_return_function_ast')
    if ($dynamicDictLookupDictReturnFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_dict_lookup_dict_return_function_ast"
    }
    $dynamicDictLookupDictReturnFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicDictLookupDictReturnFunctionCallStart + 1)
    if ($dynamicDictLookupDictReturnFunctionCallNext -lt 0) { $dynamicDictLookupDictReturnFunctionCallNext = $astWatText.Length }
    $dynamicDictLookupDictReturnFunctionCallBody = $astWatText.Substring($dynamicDictLookupDictReturnFunctionCallStart, $dynamicDictLookupDictReturnFunctionCallNext - $dynamicDictLookupDictReturnFunctionCallStart)
    if ($dynamicDictLookupDictReturnFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicDictLookupDictReturnFunctionCallBody -match 'call \$handler') {
        throw "dict-lookup-selected dict-returning function values should dispatch through the WASM function-value dispatcher"
    }
    $dynamicDictLookupDictReturnFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_dict_lookup_dict_return_function_label')
    if ($dynamicDictLookupDictReturnFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_dict_lookup_dict_return_function_label"
    }
    $dynamicDictLookupDictReturnFunctionLabelStart = [Math]::Max(0, $dynamicDictLookupDictReturnFunctionLabelIndex - 3000)
    $dynamicDictLookupDictReturnFunctionLabelWindow = $astWatText.Substring($dynamicDictLookupDictReturnFunctionLabelStart, $dynamicDictLookupDictReturnFunctionLabelIndex - $dynamicDictLookupDictReturnFunctionLabelStart)
    if ($dynamicDictLookupDictReturnFunctionLabelWindow -notmatch 'call \$call_dynamic_dict_lookup_dict_return_function_ast' -or
        $dynamicDictLookupDictReturnFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "dict-lookup-selected dict-returning function dispatch should preserve dict handles for length/field/to_str"
    }
    $dynamicArrayLookupFunctionReturnCallStart = $astWatText.IndexOf('(func $call_dynamic_array_lookup_function_return_function_ast')
    if ($dynamicArrayLookupFunctionReturnCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_array_lookup_function_return_function_ast"
    }
    $dynamicArrayLookupFunctionReturnCallNext = $astWatText.IndexOf("  (func ", $dynamicArrayLookupFunctionReturnCallStart + 1)
    if ($dynamicArrayLookupFunctionReturnCallNext -lt 0) { $dynamicArrayLookupFunctionReturnCallNext = $astWatText.Length }
    $dynamicArrayLookupFunctionReturnCallBody = $astWatText.Substring($dynamicArrayLookupFunctionReturnCallStart, $dynamicArrayLookupFunctionReturnCallNext - $dynamicArrayLookupFunctionReturnCallStart)
    if ($dynamicArrayLookupFunctionReturnCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicArrayLookupFunctionReturnCallBody -match 'call \$handler') {
        throw "array-lookup-selected function-returning function values should dispatch through the WASM function-value dispatcher"
    }
    $dynamicArrayLookupFunctionReturnLabelIndex = $astWatText.IndexOf('local.set $dynamic_array_lookup_function_return_label')
    if ($dynamicArrayLookupFunctionReturnLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_array_lookup_function_return_label"
    }
    $dynamicArrayLookupFunctionReturnLabelStart = [Math]::Max(0, $dynamicArrayLookupFunctionReturnLabelIndex - 2200)
    $dynamicArrayLookupFunctionReturnLabelWindow = $astWatText.Substring($dynamicArrayLookupFunctionReturnLabelStart, $dynamicArrayLookupFunctionReturnLabelIndex - $dynamicArrayLookupFunctionReturnLabelStart)
    if ($dynamicArrayLookupFunctionReturnLabelWindow -notmatch 'call \$__sura_call_function_2' -or
        $dynamicArrayLookupFunctionReturnLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "array-lookup-selected function-returning function dispatch should preserve returned function values for later invocation"
    }
    $dynamicBinaryDictFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_binary_dict_function_ast')
    if ($dynamicBinaryDictFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_binary_dict_function_ast"
    }
    $dynamicBinaryDictFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicBinaryDictFunctionCallStart + 1)
    if ($dynamicBinaryDictFunctionCallNext -lt 0) { $dynamicBinaryDictFunctionCallNext = $astWatText.Length }
    $dynamicBinaryDictFunctionCallBody = $astWatText.Substring($dynamicBinaryDictFunctionCallStart, $dynamicBinaryDictFunctionCallNext - $dynamicBinaryDictFunctionCallStart)
    if ($dynamicBinaryDictFunctionCallBody -notmatch 'call \$__sura_call_function_2' -or $dynamicBinaryDictFunctionCallBody -match 'call \$handler') {
        throw "two-argument function values read from homogeneous dynamic string-key dicts should dispatch through the WASM function-value dispatcher"
    }
    $dynamicBinaryDictFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_binary_dict_function_call_label')
    if ($dynamicBinaryDictFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_binary_dict_function_call_label"
    }
    $dynamicBinaryDictFunctionLabelStart = [Math]::Max(0, $dynamicBinaryDictFunctionLabelIndex - 900)
    $dynamicBinaryDictFunctionLabelWindow = $astWatText.Substring($dynamicBinaryDictFunctionLabelStart, $dynamicBinaryDictFunctionLabelIndex - $dynamicBinaryDictFunctionLabelStart)
    if ($dynamicBinaryDictFunctionLabelWindow -notmatch 'call \$call_dynamic_binary_dict_function_ast' -or
        $dynamicBinaryDictFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic two-argument dict function dispatch results should preserve the user function calls"
    }
    $dynamicBinaryArrayFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_binary_array_function_ast')
    if ($dynamicBinaryArrayFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_binary_array_function_ast"
    }
    $dynamicBinaryArrayFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicBinaryArrayFunctionCallStart + 1)
    if ($dynamicBinaryArrayFunctionCallNext -lt 0) { $dynamicBinaryArrayFunctionCallNext = $astWatText.Length }
    $dynamicBinaryArrayFunctionCallBody = $astWatText.Substring($dynamicBinaryArrayFunctionCallStart, $dynamicBinaryArrayFunctionCallNext - $dynamicBinaryArrayFunctionCallStart)
    if ($dynamicBinaryArrayFunctionCallBody -notmatch 'call \$__sura_call_function_2' -or $dynamicBinaryArrayFunctionCallBody -match 'call \$handler') {
        throw "two-argument function values read from homogeneous dynamic numeric-index arrays should dispatch through the WASM function-value dispatcher"
    }
    $dynamicBinaryArrayFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_binary_array_function_call_label')
    if ($dynamicBinaryArrayFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_binary_array_function_call_label"
    }
    $dynamicBinaryArrayFunctionLabelStart = [Math]::Max(0, $dynamicBinaryArrayFunctionLabelIndex - 900)
    $dynamicBinaryArrayFunctionLabelWindow = $astWatText.Substring($dynamicBinaryArrayFunctionLabelStart, $dynamicBinaryArrayFunctionLabelIndex - $dynamicBinaryArrayFunctionLabelStart)
    if ($dynamicBinaryArrayFunctionLabelWindow -notmatch 'call \$call_dynamic_binary_array_function_ast' -or
        $dynamicBinaryArrayFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic two-argument array function dispatch results should preserve the user function calls"
    }
    $foreachBinaryFunctionCallStart = $astWatText.IndexOf('(func $call_foreach_binary_function_ast')
    if ($foreachBinaryFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_foreach_binary_function_ast"
    }
    $foreachBinaryFunctionCallNext = $astWatText.IndexOf("  (func ", $foreachBinaryFunctionCallStart + 1)
    if ($foreachBinaryFunctionCallNext -lt 0) { $foreachBinaryFunctionCallNext = $astWatText.Length }
    $foreachBinaryFunctionCallBody = $astWatText.Substring($foreachBinaryFunctionCallStart, $foreachBinaryFunctionCallNext - $foreachBinaryFunctionCallStart)
    if ($foreachBinaryFunctionCallBody -notmatch 'call \$__sura_call_function_2' -or $foreachBinaryFunctionCallBody -match 'call \$handler') {
        throw "two-argument function-valued foreach loop variables should dispatch through the WASM function-value dispatcher"
    }
    $foreachBinaryFunctionLabelIndex = $astWatText.IndexOf('local.set $foreach_binary_function_call_label')
    if ($foreachBinaryFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include foreach_binary_function_call_label"
    }
    $foreachBinaryFunctionLabelStart = [Math]::Max(0, $foreachBinaryFunctionLabelIndex - 900)
    $foreachBinaryFunctionLabelWindow = $astWatText.Substring($foreachBinaryFunctionLabelStart, $foreachBinaryFunctionLabelIndex - $foreachBinaryFunctionLabelStart)
    if ($foreachBinaryFunctionLabelWindow -notmatch 'call \$call_foreach_binary_function_ast' -or
        $foreachBinaryFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of foreach two-argument function dispatch results should preserve the user function calls"
    }
    $dynamicBinaryParamFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_binary_param_function_ast')
    if ($dynamicBinaryParamFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_binary_param_function_ast"
    }
    $dynamicBinaryParamFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicBinaryParamFunctionCallStart + 1)
    if ($dynamicBinaryParamFunctionCallNext -lt 0) { $dynamicBinaryParamFunctionCallNext = $astWatText.Length }
    $dynamicBinaryParamFunctionCallBody = $astWatText.Substring($dynamicBinaryParamFunctionCallStart, $dynamicBinaryParamFunctionCallNext - $dynamicBinaryParamFunctionCallStart)
    if ($dynamicBinaryParamFunctionCallBody -notmatch 'call \$__sura_call_function_2' -or $dynamicBinaryParamFunctionCallBody -match 'call \$handler') {
        throw "two-argument function-valued parameters with multiple observed lifted targets should dispatch through the WASM function-value dispatcher"
    }
    $dynamicBinaryParamFunctionLabelIndex = $astWatText.IndexOf('local.set $dynamic_binary_param_function_call_label')
    if ($dynamicBinaryParamFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include dynamic_binary_param_function_call_label"
    }
    $dynamicBinaryParamFunctionLabelStart = [Math]::Max(0, $dynamicBinaryParamFunctionLabelIndex - 900)
    $dynamicBinaryParamFunctionLabelWindow = $astWatText.Substring($dynamicBinaryParamFunctionLabelStart, $dynamicBinaryParamFunctionLabelIndex - $dynamicBinaryParamFunctionLabelStart)
    if ($dynamicBinaryParamFunctionLabelWindow -notmatch 'call \$call_dynamic_binary_param_function_ast' -or
        $dynamicBinaryParamFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of dynamic two-argument parameter function dispatch results should preserve the user function calls"
    }
    $dynamicDictFunctionCallStart = $astWatText.IndexOf('(func $call_dynamic_dict_function_key_ast')
    if ($dynamicDictFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include call_dynamic_dict_function_key_ast"
    }
    $dynamicDictFunctionCallNext = $astWatText.IndexOf("  (func ", $dynamicDictFunctionCallStart + 1)
    if ($dynamicDictFunctionCallNext -lt 0) { $dynamicDictFunctionCallNext = $astWatText.Length }
    $dynamicDictFunctionCallBody = $astWatText.Substring($dynamicDictFunctionCallStart, $dynamicDictFunctionCallNext - $dynamicDictFunctionCallStart)
    if ($dynamicDictFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $dynamicDictFunctionCallBody -match 'call \$handler') {
        throw "function values read from homogeneous dynamic string-key dicts should dispatch through the WASM function-value dispatcher"
    }
    $directDynamicDictFunctionLabelIndex = $astWatText.IndexOf('local.set $direct_dynamic_dict_function_call_label')
    if ($directDynamicDictFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include direct_dynamic_dict_function_call_label"
    }
    $directDynamicDictFunctionLabelStart = [Math]::Max(0, $directDynamicDictFunctionLabelIndex - 520)
    $directDynamicDictFunctionLabelWindow = $astWatText.Substring($directDynamicDictFunctionLabelStart, $directDynamicDictFunctionLabelIndex - $directDynamicDictFunctionLabelStart)
    if ($directDynamicDictFunctionLabelWindow -notmatch 'call \$call_dynamic_dict_function_key_ast' -or
        $directDynamicDictFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of function-value dict dispatch results should preserve the user function call instead of falling back to unreachable"
    }
    $methodDynamicDictFunctionLabelIndex = $astWatText.IndexOf('local.set $function_holder_dynamic_dict_call_label')
    if ($methodDynamicDictFunctionLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_dynamic_dict_call_label"
    }
    $methodDynamicDictFunctionLabelStart = [Math]::Max(0, $methodDynamicDictFunctionLabelIndex - 560)
    $methodDynamicDictFunctionLabelWindow = $astWatText.Substring($methodDynamicDictFunctionLabelStart, $methodDynamicDictFunctionLabelIndex - $methodDynamicDictFunctionLabelStart)
    if ($methodDynamicDictFunctionLabelWindow -notmatch 'call \$__sura_method_AstFunctionHolder_dict_handler_call' -or
        $methodDynamicDictFunctionLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of method-returned function-value dict dispatch results should preserve the method call instead of falling back to unreachable"
    }
    $methodDynamicDictProfileBodyStart = $astWatText.IndexOf('(func $__sura_method_AstFunctionHolder_dict_handler_profile')
    if ($methodDynamicDictProfileBodyStart -lt 0) {
        throw "generated AST JSON WASM should include AstFunctionHolder.dict_handler_profile"
    }
    $methodDynamicDictProfileBodyNext = $astWatText.IndexOf("  (func ", $methodDynamicDictProfileBodyStart + 1)
    if ($methodDynamicDictProfileBodyNext -lt 0) { $methodDynamicDictProfileBodyNext = $astWatText.Length }
    $methodDynamicDictProfileBody = $astWatText.Substring($methodDynamicDictProfileBodyStart, $methodDynamicDictProfileBodyNext - $methodDynamicDictProfileBodyStart)
    if ($methodDynamicDictProfileBody -notmatch 'call \$__sura_call_function_1' -or $methodDynamicDictProfileBody -match 'call \$handler') {
        throw "method-local dict-lookup-selected dict-returning function values should dispatch through the WASM function-value dispatcher"
    }
    $methodDynamicDictProfileLabelIndex = $astWatText.IndexOf('local.set $function_holder_dynamic_dict_profile_label')
    if ($methodDynamicDictProfileLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_dynamic_dict_profile_label"
    }
    $methodDynamicDictProfileLabelStart = [Math]::Max(0, $methodDynamicDictProfileLabelIndex - 3000)
    $methodDynamicDictProfileLabelWindow = $astWatText.Substring($methodDynamicDictProfileLabelStart, $methodDynamicDictProfileLabelIndex - $methodDynamicDictProfileLabelStart)
    if ($methodDynamicDictProfileLabelWindow -notmatch 'call \$__sura_method_AstFunctionHolder_dict_handler_profile' -or
        $methodDynamicDictProfileLabelWindow -notmatch '(?s)call \$__sura_method_AstFunctionHolder_dict_handler_profile\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_to_string' -or
        $methodDynamicDictProfileLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "method-local dict-lookup-selected dict-returning function dispatch should preserve dict handles for length/field/to_str"
    }
    $methodArrayLookupFunctionReturnBodyStart = $astWatText.IndexOf('(func $__sura_method_AstFunctionHolder_array_handler_function')
    if ($methodArrayLookupFunctionReturnBodyStart -lt 0) {
        throw "generated AST JSON WASM should include AstFunctionHolder.array_handler_function"
    }
    $methodArrayLookupFunctionReturnBodyNext = $astWatText.IndexOf("  (func ", $methodArrayLookupFunctionReturnBodyStart + 1)
    if ($methodArrayLookupFunctionReturnBodyNext -lt 0) { $methodArrayLookupFunctionReturnBodyNext = $astWatText.Length }
    $methodArrayLookupFunctionReturnBody = $astWatText.Substring($methodArrayLookupFunctionReturnBodyStart, $methodArrayLookupFunctionReturnBodyNext - $methodArrayLookupFunctionReturnBodyStart)
    if ($methodArrayLookupFunctionReturnBody -notmatch 'call \$__sura_call_function_1' -or $methodArrayLookupFunctionReturnBody -match 'call \$handler') {
        throw "method-local array-lookup-selected function-returning function values should dispatch through the WASM function-value dispatcher"
    }
    $methodArrayLookupFunctionReturnLabelIndex = $astWatText.IndexOf('local.set $function_holder_lookup_return_label')
    if ($methodArrayLookupFunctionReturnLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_lookup_return_label"
    }
    $methodArrayLookupFunctionReturnCallIndex = $astWatText.IndexOf('local.set $function_holder_lookup_return_call')
    if ($methodArrayLookupFunctionReturnCallIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_lookup_return_call"
    }
    $methodArrayLookupFunctionReturnCallStart = [Math]::Max(0, $methodArrayLookupFunctionReturnCallIndex - 520)
    $methodArrayLookupFunctionReturnCallWindow = $astWatText.Substring($methodArrayLookupFunctionReturnCallStart, $methodArrayLookupFunctionReturnCallIndex - $methodArrayLookupFunctionReturnCallStart)
    if ($methodArrayLookupFunctionReturnCallWindow -notmatch 'call \$__sura_call_function_2' -or
        $methodArrayLookupFunctionReturnCallWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "method-local array-lookup-selected function-returning dispatch should preserve returned function values for later invocation"
    }
    $superObjectParamTypeLabelIndex = $astWatText.IndexOf('local.set $super_object_param_type_label')
    if ($superObjectParamTypeLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include super_object_param_type_label"
    }
    $superObjectParamTypeLabelStart = [Math]::Max(0, $superObjectParamTypeLabelIndex - 640)
    $superObjectParamTypeLabelWindow = $astWatText.Substring($superObjectParamTypeLabelStart, $superObjectParamTypeLabelIndex - $superObjectParamTypeLabelStart)
    if ($superObjectParamTypeLabelWindow -notmatch 'call \$__sura_method_AstForwardChild_child_label_type' -or
        $superObjectParamTypeLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "string interpolation of super.method object-param return types should lower through the child method instead of falling back to unreachable"
    }
    $superObjectParamTypeBodyStart = $astWatText.IndexOf('(func $__sura_method_AstForwardChild_child_label_type')
    if ($superObjectParamTypeBodyStart -lt 0) {
        throw "generated AST JSON WASM should include AstForwardChild.child_label_type"
    }
    $superObjectParamTypeBodyNext = $astWatText.IndexOf("  (func ", $superObjectParamTypeBodyStart + 1)
    if ($superObjectParamTypeBodyNext -lt 0) { $superObjectParamTypeBodyNext = $astWatText.Length }
    $superObjectParamTypeBody = $astWatText.Substring($superObjectParamTypeBodyStart, $superObjectParamTypeBodyNext - $superObjectParamTypeBodyStart)
    if ($superObjectParamTypeBody -notmatch 'call \$__sura_method_AstForwardParent_forwarded_label' -or
        $superObjectParamTypeBody -notmatch 'call \$__sura_value_type_name' -or
        $superObjectParamTypeBody -match 'unsupported WASM AST method call') {
        throw "super.method inside interpolation type(...) should preserve parent dispatch and dynamic type-name lowering"
    }
    $superObjectParamArrayLenLabelIndex = $astWatText.IndexOf('local.set $super_object_param_array_len_label')
    if ($superObjectParamArrayLenLabelIndex -lt 0) {
        throw "generated AST JSON WASM should include super_object_param_array_len_label"
    }
    $superObjectParamArrayLenLabelStart = [Math]::Max(0, $superObjectParamArrayLenLabelIndex - 640)
    $superObjectParamArrayLenLabelWindow = $astWatText.Substring($superObjectParamArrayLenLabelStart, $superObjectParamArrayLenLabelIndex - $superObjectParamArrayLenLabelStart)
    if ($superObjectParamArrayLenLabelWindow -notmatch 'call \$__sura_method_AstForwardArrayChild_child_values_len' -or
        $superObjectParamArrayLenLabelWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "string interpolation of length(super.method object-param array return) should preserve the child method instead of falling back to unreachable"
    }
    $superObjectParamArrayLenBodyStart = $astWatText.IndexOf('(func $__sura_method_AstForwardArrayChild_child_values_len')
    if ($superObjectParamArrayLenBodyStart -lt 0) {
        throw "generated AST JSON WASM should include AstForwardArrayChild.child_values_len"
    }
    $superObjectParamArrayLenBodyNext = $astWatText.IndexOf("  (func ", $superObjectParamArrayLenBodyStart + 1)
    if ($superObjectParamArrayLenBodyNext -lt 0) { $superObjectParamArrayLenBodyNext = $astWatText.Length }
    $superObjectParamArrayLenBody = $astWatText.Substring($superObjectParamArrayLenBodyStart, $superObjectParamArrayLenBodyNext - $superObjectParamArrayLenBodyStart)
    if ($superObjectParamArrayLenBody -notmatch 'call \$__sura_method_AstForwardArrayParent_forwarded_values' -or
        $superObjectParamArrayLenBody -notmatch 'call \$__sura_value_array' -or
        $superObjectParamArrayLenBody -notmatch 'call \$__sura_value_length' -or
        $superObjectParamArrayLenBody -match 'unsupported WASM AST length') {
        throw "super.method inside interpolation length(...) should preserve parent dispatch and object-param-derived array return lowering"
    }
    if ($astWatText -notmatch '\(func \$__sura_call_function_1 \(param \$id i32\) \(param \$arg0 i32\) \(result i32\)' -or
        $astWatText -notmatch '(?s)\(func \$__sura_call_function_1.*?call \$block_double_ast.*?call \$triple_ast') {
        throw "WASM function-value dispatcher should include same-arity lifted function targets"
    }
    $fieldFunctionCallStart = $astWatText.IndexOf('(func $__sura_method_AstFunctionHolder_call_handler')
    if ($fieldFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include AstFunctionHolder.call_handler"
    }
    $fieldFunctionCallNext = $astWatText.IndexOf("  (func ", $fieldFunctionCallStart + 1)
    if ($fieldFunctionCallNext -lt 0) { $fieldFunctionCallNext = $astWatText.Length }
    $fieldFunctionCallBody = $astWatText.Substring($fieldFunctionCallStart, $fieldFunctionCallNext - $fieldFunctionCallStart)
    if ($fieldFunctionCallBody -notmatch 'call \$__sura_call_function_1' -or $fieldFunctionCallBody -match 'call \$picked') {
        throw "function-valued object fields with multiple observed lifted targets should dispatch through the WASM function-value dispatcher"
    }
    $binaryFieldFunctionCallStart = $astWatText.IndexOf('(func $__sura_method_AstBinaryFunctionHolder_call_handler')
    if ($binaryFieldFunctionCallStart -lt 0) {
        throw "generated AST JSON WASM should include AstBinaryFunctionHolder.call_handler"
    }
    $binaryFieldFunctionCallNext = $astWatText.IndexOf("  (func ", $binaryFieldFunctionCallStart + 1)
    if ($binaryFieldFunctionCallNext -lt 0) { $binaryFieldFunctionCallNext = $astWatText.Length }
    $binaryFieldFunctionCallBody = $astWatText.Substring($binaryFieldFunctionCallStart, $binaryFieldFunctionCallNext - $binaryFieldFunctionCallStart)
    if ($binaryFieldFunctionCallBody -notmatch 'call \$__sura_call_function_2' -or $binaryFieldFunctionCallBody -match 'call \$picked') {
        throw "two-argument function-valued object fields should dispatch through the WASM function-value dispatcher"
    }
    $binaryMethodReturnCallIndex = $astWatText.IndexOf('local.set $binary_function_holder_chosen_call')
    if ($binaryMethodReturnCallIndex -lt 0) {
        throw "generated AST JSON WASM should include binary_function_holder_chosen_call"
    }
    $binaryMethodReturnCallStart = [Math]::Max(0, $binaryMethodReturnCallIndex - 520)
    $binaryMethodReturnCallWindow = $astWatText.Substring($binaryMethodReturnCallStart, $binaryMethodReturnCallIndex - $binaryMethodReturnCallStart)
    if ($binaryMethodReturnCallWindow -notmatch 'call \$__sura_call_function_2' -or $binaryMethodReturnCallWindow -match 'call \$binary_function_holder_chosen_handler') {
        throw "two-argument function-valued method returns assigned to a local alias should dispatch through the WASM function-value dispatcher"
    }
    $binaryMethodDictCallIndex = $astWatText.IndexOf('local.set $binary_function_holder_dict_call_label')
    if ($binaryMethodDictCallIndex -lt 0) {
        throw "generated AST JSON WASM should include binary_function_holder_dict_call_label"
    }
    $binaryMethodDictCallStart = [Math]::Max(0, $binaryMethodDictCallIndex - 900)
    $binaryMethodDictCallWindow = $astWatText.Substring($binaryMethodDictCallStart, $binaryMethodDictCallIndex - $binaryMethodDictCallStart)
    if ($binaryMethodDictCallWindow -notmatch 'call \$__sura_method_AstBinaryFunctionHolder_dict_handler_call' -or
        $binaryMethodDictCallWindow -match '(?s)drop\s+unreachable\s+i32\.const 0') {
        throw "direct stringification of method-local two-argument dict function dispatch should preserve the method calls"
    }
    $binarySuperReturnCallIndex = $astWatText.IndexOf('local.set $binary_function_child_call')
    if ($binarySuperReturnCallIndex -lt 0) {
        throw "generated AST JSON WASM should include binary_function_child_call"
    }
    $binarySuperReturnCallStart = [Math]::Max(0, $binarySuperReturnCallIndex - 620)
    $binarySuperReturnCallWindow = $astWatText.Substring($binarySuperReturnCallStart, $binarySuperReturnCallIndex - $binarySuperReturnCallStart)
    if ($binarySuperReturnCallWindow -notmatch 'call \$__sura_call_function_2' -or $binarySuperReturnCallWindow -match 'call \$binary_function_child_handler') {
        throw "two-argument function-valued super.method returns assigned to local aliases should dispatch through the WASM function-value dispatcher"
    }
    $binarySuperDirectCallStart = $astWatText.IndexOf('(func $__sura_method_AstBinaryFunctionChild_inherited_call')
    if ($binarySuperDirectCallStart -lt 0) {
        throw "generated AST JSON WASM should include AstBinaryFunctionChild.inherited_call"
    }
    $binarySuperDirectCallNext = $astWatText.IndexOf("  (func ", $binarySuperDirectCallStart + 1)
    if ($binarySuperDirectCallNext -lt 0) { $binarySuperDirectCallNext = $astWatText.Length }
    $binarySuperDirectCallBody = $astWatText.Substring($binarySuperDirectCallStart, $binarySuperDirectCallNext - $binarySuperDirectCallStart)
    if ($binarySuperDirectCallBody -notmatch 'call \$__sura_method_AstBinaryFunctionHolder_choose_handler' -or
        $binarySuperDirectCallBody -notmatch 'call \$__sura_call_function_2' -or
        $binarySuperDirectCallBody -match 'call \$picked') {
        throw "two-argument function-valued super.method aliases should preserve parent dispatch and indirect invocation"
    }
    $returnFunctionCallIndex = $astWatText.IndexOf('local.set $function_holder_return_call')
    if ($returnFunctionCallIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_return_call"
    }
    $returnFunctionCallStart = [Math]::Max(0, $returnFunctionCallIndex - 420)
    $returnFunctionCallWindow = $astWatText.Substring($returnFunctionCallStart, $returnFunctionCallIndex - $returnFunctionCallStart)
    if ($returnFunctionCallWindow -notmatch 'call \$__sura_call_function_1' -or $returnFunctionCallWindow -match 'call \$function_holder_returned_handler') {
        throw "function-valued method returns assigned to a local alias should dispatch through the WASM function-value dispatcher when multiple targets are observed"
    }
    $literalFunctionCallIndex = $astWatText.IndexOf('local.set $function_pick_true_call')
    if ($literalFunctionCallIndex -lt 0) {
        throw "generated AST JSON WASM should include function_pick_true_call"
    }
    $literalFunctionCallStart = [Math]::Max(0, $literalFunctionCallIndex - 420)
    $literalFunctionCallWindow = $astWatText.Substring($literalFunctionCallStart, $literalFunctionCallIndex - $literalFunctionCallStart)
    if ($literalFunctionCallWindow -notmatch '(call \$block_double_ast|i32\.const 2\s+i32\.mul)' -or $literalFunctionCallWindow -match 'call \$function_pick_true') {
        throw "function-valued literal-branch function returns should lower through the selected lifted WASM function or its single-return inline body, not the local alias"
    }
    $literalMethodFunctionCallIndex = $astWatText.IndexOf('local.set $function_holder_chosen_call')
    if ($literalMethodFunctionCallIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_chosen_call"
    }
    $literalMethodFunctionCallStart = [Math]::Max(0, $literalMethodFunctionCallIndex - 420)
    $literalMethodFunctionCallWindow = $astWatText.Substring($literalMethodFunctionCallStart, $literalMethodFunctionCallIndex - $literalMethodFunctionCallStart)
    if ($literalMethodFunctionCallWindow -notmatch 'call \$__sura_call_function_1' -or $literalMethodFunctionCallWindow -match 'call \$function_holder_chosen_handler') {
        throw "function-valued literal-branch method returns should dispatch through the WASM function-value dispatcher when multiple targets are observed"
    }
    $matchFunctionCallIndex = $astWatText.IndexOf('local.set $function_match_pick_call')
    if ($matchFunctionCallIndex -lt 0) {
        throw "generated AST JSON WASM should include function_match_pick_call"
    }
    $matchFunctionCallStart = [Math]::Max(0, $matchFunctionCallIndex - 420)
    $matchFunctionCallWindow = $astWatText.Substring($matchFunctionCallStart, $matchFunctionCallIndex - $matchFunctionCallStart)
    if ($matchFunctionCallWindow -notmatch '(call \$block_double_ast|i32\.const 2\s+i32\.mul)' -or $matchFunctionCallWindow -match 'call \$function_match_pick') {
        throw "function-valued literal match function returns should lower through the selected lifted WASM function or its single-return inline body, not the local alias"
    }
    $matchMethodFunctionCallIndex = $astWatText.IndexOf('local.set $function_holder_match_call')
    if ($matchMethodFunctionCallIndex -lt 0) {
        throw "generated AST JSON WASM should include function_holder_match_call"
    }
    $matchMethodFunctionCallStart = [Math]::Max(0, $matchMethodFunctionCallIndex - 420)
    $matchMethodFunctionCallWindow = $astWatText.Substring($matchMethodFunctionCallStart, $matchMethodFunctionCallIndex - $matchMethodFunctionCallStart)
    if ($matchMethodFunctionCallWindow -notmatch 'call \$__sura_call_function_1' -or $matchMethodFunctionCallWindow -match 'call \$function_holder_match_handler') {
        throw "function-valued literal match method returns should dispatch through the WASM function-value dispatcher when multiple targets are observed"
    }
    foreach ($loopConflictName in @(
        "repeat_conflict_after", "repeat_conflict_field_after",
        "while_conflict_after", "while_conflict_field_after",
        "for_conflict_after", "for_conflict_field_after",
        "foreach_array_conflict_after", "foreach_array_conflict_field_after",
        "foreach_dict_conflict_after", "foreach_dict_conflict_field_after"
    )) {
        $afterIndex = $astWatText.IndexOf("local.set `$$loopConflictName")
        if ($afterIndex -lt 0) {
            throw "generated AST JSON WASM should include $loopConflictName assignment"
        }
        $windowStart = [Math]::Max(0, $afterIndex - 320)
        $window = $astWatText.Substring($windowStart, $afterIndex - $windowStart)
        if ($window -match 'call \$__sura_string_concat' -or $window -notmatch 'i32\.add') {
            throw "AST JSON WASM should merge loop body hints with the pre-loop state before $loopConflictName"
        }
    }
    foreach ($shortCircuitName in @("short_circuit_and_ok", "short_circuit_or_ok")) {
        $afterIndex = $astWatText.IndexOf("local.set `$$shortCircuitName")
        if ($afterIndex -lt 0) {
            throw "generated AST JSON WASM should include $shortCircuitName assignment"
        }
        $windowStart = [Math]::Max(0, $afterIndex - 600)
        $window = $astWatText.Substring($windowStart, $afterIndex - $windowStart)
        if ($window -notmatch 'if \(result i32\)' -or $window -notmatch '\belse\b' -or $window -notmatch 'call \$__sura_value_div') {
            throw "AST JSON WASM should lower $shortCircuitName with branch-only right operand evaluation"
        }
    }
    if ($astWatText -notmatch '(?s)local\.get \$compound_numbers\s+i32\.const 1\s+local\.get \$compound_numbers\s+i32\.const 1\s+call \$__sura_array_get_checked\s+i32\.const 4\s+i32\.add\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$mutable_profile.*?call \$__sura_make_array_\d+\s+local\.get \$mutable_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+i32\.const 5\s+i32\.add\s+call \$__sura_dict_put\s+local\.set \$mutable_profile' -or
        $astWatText -notmatch '(?s)local\.get \$mixed_num_dict\s+local\.get \$mixed_num_key_lookup\s+local\.get \$mixed_num_dict\s+local\.get \$mixed_num_key_lookup\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+i32\.const 2\s+i32\.add\s+call \$__sura_dict_put\s+local\.set \$mixed_num_dict') {
        throw "AST JSON WASM should lower compound index, dynamic-key dict, and dot assignments through read-modify-write target assignment"
    }
    $compoundOpsIndex = $astWatText.IndexOf('local.set $compound_ops_label')
    if ($compoundOpsIndex -lt 0) {
        throw "generated AST JSON WASM should include compound_ops_label assignment"
    }
    $compoundOpsStart = [Math]::Max(0, $compoundOpsIndex - 2200)
    $compoundOpsWindow = $astWatText.Substring($compoundOpsStart, $compoundOpsIndex - $compoundOpsStart)
    if ($compoundOpsWindow -notmatch '(?s)local\.get \$compound_op_score\s+i32\.const 4\s+i32\.sub\s+local\.set \$compound_op_score' -or
        $compoundOpsWindow -notmatch '(?s)local\.get \$compound_op_score\s+i32\.const 3\s+i32\.mul\s+local\.set \$compound_op_score' -or
        $compoundOpsWindow -notmatch '(?s)local\.get \$compound_op_score\s+call \$__sura_value_num\s+local\.set \$__sura_wasm_value_tmp\s+local\.get \$__sura_wasm_value_tmp\s+local\.set \$compound_op_score.*?local\.get \$__sura_wasm_value_tmp\s+i32\.const 2\s+call \$__sura_value_num\s+call \$__sura_value_div.*?local\.set \$compound_op_score' -or
        $compoundOpsWindow -notmatch '(?s)local\.get \$compound_op_score\s+i32\.const 10\s+call \$__sura_value_num\s+call \$__sura_value_mod.*?local\.set \$compound_op_score' -or
        $compoundOpsWindow -notmatch '(?s)local\.get \$compound_op_score\s+call \$__sura_value_to_string\s+call \$__sura_string_concat') {
        throw "AST JSON WASM should lower numeric -=, *=, /=, and %= in-place operators while preserving tagged real division and later numeric stringification"
    }
    if ($astWatText -notmatch '(?s)local\.get \$compound_names\s+i32\.const 0\s+local\.get \$compound_names\s+i32\.const 0\s+call \$__sura_array_get_checked\s+.*?call \$__sura_string_concat\s+call \$__sura_array_set_checked' -or
        $astWatText -notmatch '(?s)local\.get \$compound_profile.*?call \$__sura_make_array_\d+\s+local\.get \$compound_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+.*?call \$__sura_string_concat\s+call \$__sura_dict_put\s+local\.set \$compound_profile' -or
        $astWatText -notmatch '(?s)local\.get \$compound_profile\s+local\.get \$compound_key\s+local\.get \$compound_profile\s+local\.get \$compound_key\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+.*?call \$__sura_string_concat\s+call \$__sura_dict_put\s+local\.set \$compound_profile') {
        throw "AST JSON WASM should lower string compound index, dynamic-key dict, and dot assignments through string read-modify-write"
    }
    $astFieldAssignIndex = $astWatText.IndexOf('local.set $field_assign_label')
    if ($astFieldAssignIndex -lt 0) {
        throw "generated AST JSON WASM should include field assignment label"
    }
    $astFieldAssignStart = [Math]::Max(0, $astFieldAssignIndex - 70000)
    $astFieldAssignWindow = $astWatText.Substring($astFieldAssignStart, $astFieldAssignIndex - $astFieldAssignStart)
    $astFieldSetCount = [regex]::Matches($astFieldAssignWindow, '(?s)local\.get \$field_values\s+i32\.const \d+.*?call \$__sura_dict_put\s+local\.set \$field_values').Count
    if ($astFieldSetCount -lt 6 -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_string.*?call \$__sura_value_type_name' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_string.*?call \$__sura_value_to_string' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_bool.*?call \$__sura_value_type_name' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_bool.*?call \$__sura_value_is_truthy' -or
        $astFieldAssignWindow -notmatch 'call \$__sura_value_nil\s+call \$__sura_value_type_name' -or
        $astFieldAssignWindow -notmatch 'call \$__sura_value_nil\s+call \$__sura_value_is_truthy' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_array.*?call \$__sura_value_type_name' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_dict.*?call \$__sura_value_type_name' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_num.*?call \$__sura_value_type_name' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get.*?call \$__sura_value_num.*?call \$__sura_value_is_truthy' -or
        $astFieldAssignWindow -notmatch '(?s)local\.get \$field_values\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_to_string') {
        throw "AST JSON WASM should lower field assignments for string/bool/nil/array/dict/number values and preserve later Value helpers"
    }
    $ternaryLiteralIndex = $astWatText.IndexOf('local.set $ternary_literal_to_str_label')
    if ($ternaryLiteralIndex -lt 0) {
        throw "generated AST JSON WASM should include ternary literal to_str label lowering"
    }
    $ternaryLiteralStart = [Math]::Max(0, $ternaryLiteralIndex - 8000)
    $ternaryLiteralWindow = $astWatText.Substring($ternaryLiteralStart, $ternaryLiteralIndex - $ternaryLiteralStart)
    if ($ternaryLiteralWindow -notmatch 'if \(result i32\)' -or
        ([regex]::Matches($ternaryLiteralWindow, 'call \$__sura_value_dynamic_array').Count -lt 2) -or
        $ternaryLiteralWindow -notmatch '\belse\b' -or
        ([regex]::Matches($ternaryLiteralWindow, 'call \$__sura_value_to_string').Count -lt 2)) {
        throw "ternary array literal to_str should lower both branches through dynamic Value collection stringification"
    }
    $arrayVarToStrIndex = $astWatText.IndexOf('local.set $array_var_to_str_label')
    if ($arrayVarToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include homogeneous array variable to_str label lowering"
    }
    $arrayVarToStrStart = [Math]::Max(0, $arrayVarToStrIndex - 12000)
    $arrayVarToStrWindow = $astWatText.Substring($arrayVarToStrStart, $arrayVarToStrIndex - $arrayVarToStrStart)
    if ($arrayVarToStrWindow -notmatch 'call \$__sura_array_to_string_num' -or
        $arrayVarToStrWindow -notmatch 'call \$__sura_array_to_string_string' -or
        $arrayVarToStrWindow -notmatch 'call \$__sura_array_to_string_bool') {
        throw "homogeneous array variable to_str should lower through typed array stringification helpers"
    }
    $mixedArrayVarToStrIndex = $astWatText.IndexOf('local.set $mixed_array_var_to_str_label')
    if ($mixedArrayVarToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include mixed primitive array variable to_str label lowering"
    }
    $mixedArrayVarToStrStart = [Math]::Max(0, $mixedArrayVarToStrIndex - 16000)
    $mixedArrayVarToStrWindow = $astWatText.Substring($mixedArrayVarToStrStart, $mixedArrayVarToStrIndex - $mixedArrayVarToStrStart)
    if ($mixedArrayVarToStrWindow -notmatch '(?s)local\.get \$mixed_values\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
        $mixedArrayVarToStrWindow -notmatch '(?s)local\.get \$mixed_values\s+i32\.const 1\s+call \$__sura_array_get_checked' -or
        $mixedArrayVarToStrWindow -notmatch '(?s)local\.get \$mixed_values\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\)' -or
        $mixedArrayVarToStrWindow -notmatch '(?s)local\.get \$mixed_values\s+i32\.const 3\s+call \$__sura_array_get_checked\s+drop') {
        throw "mixed primitive array variable to_str should lower fixed indexed elements with per-index conversions"
    }
    $mixedReturnArrayIndex = $astWatText.IndexOf('local.set $mixed_function_return_array_label')
    if ($mixedReturnArrayIndex -lt 0) {
        throw "generated AST JSON WASM should include mixed array function-return label lowering"
    }
    $mixedReturnArrayStart = [Math]::Max(0, $mixedReturnArrayIndex - 16000)
    $mixedReturnArrayWindow = $astWatText.Substring($mixedReturnArrayStart, $mixedReturnArrayIndex - $mixedReturnArrayStart)
    if ($mixedReturnArrayWindow -notmatch '(?s)local\.get \$mixed_values_from_func\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
        $mixedReturnArrayWindow -notmatch '(?s)local\.get \$mixed_values_from_func\s+i32\.const 1\s+call \$__sura_array_get_checked' -or
        $mixedReturnArrayWindow -notmatch '(?s)local\.get \$mixed_values_from_func\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\)' -or
        $mixedReturnArrayWindow -notmatch '(?s)local\.get \$mixed_values_from_func\s+i32\.const 3\s+call \$__sura_array_get_checked\s+drop') {
        throw "mixed primitive array function returns should propagate fixed indexed element conversions"
    }
    $directMixedReturnArrayIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_array_label')
    if ($directMixedReturnArrayIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return indexed label lowering"
    }
    $directMixedReturnArrayStart = [Math]::Max(0, $directMixedReturnArrayIndex - 20000)
    $directMixedReturnArrayWindow = $astWatText.Substring($directMixedReturnArrayStart, $directMixedReturnArrayIndex - $directMixedReturnArrayStart)
    if ($directMixedReturnArrayWindow -notmatch '(?s)call \$make_mixed_values_ast.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const 0\s+call \$__sura_array_get_checked\s+drop' -or
        $directMixedReturnArrayWindow -notmatch '(?s)call \$make_mixed_values_ast.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const 1\s+call \$__sura_array_get_checked' -or
        $directMixedReturnArrayWindow -notmatch '(?s)call \$make_mixed_values_ast.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\)' -or
        $directMixedReturnArrayWindow -notmatch '(?s)call \$make_mixed_values_ast.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const 3\s+call \$__sura_array_get_checked\s+drop') {
        throw "direct mixed primitive array function returns should propagate fixed indexed element conversions"
    }
    $directMixedReturnArrayToStrIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_array_to_str_label')
    if ($directMixedReturnArrayToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return array to_str label lowering"
    }
    $directMixedReturnArrayToStrStart = $astWatText.LastIndexOf('call $make_mixed_values_ast', $directMixedReturnArrayToStrIndex)
    if ($directMixedReturnArrayToStrStart -lt 0) { $directMixedReturnArrayToStrStart = [Math]::Max(0, $directMixedReturnArrayToStrIndex - 28000) }
    $directMixedReturnArrayToStrWindow = $astWatText.Substring($directMixedReturnArrayToStrStart, $directMixedReturnArrayToStrIndex - $directMixedReturnArrayToStrStart)
    if (([regex]::Matches($directMixedReturnArrayToStrWindow, 'call \$make_mixed_values_ast').Count -ne 1) -or
        $directMixedReturnArrayToStrWindow -notmatch '(?s)call \$make_mixed_values_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp' -or
        ([regex]::Matches($directMixedReturnArrayToStrWindow, 'local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_array_get_checked').Count -lt 4) -or
        $directMixedReturnArrayToStrWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
        $directMixedReturnArrayToStrWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\)' -or
        $directMixedReturnArrayToStrWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 3\s+call \$__sura_array_get_checked\s+drop') {
        throw "direct mixed primitive array function returns should stringify fixed indexes with per-element conversions"
    }
    $directMixedReturnArrayInterpIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_array_interp_label')
    if ($directMixedReturnArrayInterpIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return array interpolation label lowering"
    }
    $directMixedReturnArrayInterpStart = $astWatText.LastIndexOf('call $make_mixed_values_ast', $directMixedReturnArrayInterpIndex)
    if ($directMixedReturnArrayInterpStart -lt 0) { $directMixedReturnArrayInterpStart = [Math]::Max(0, $directMixedReturnArrayInterpIndex - 28000) }
    $directMixedReturnArrayInterpWindow = $astWatText.Substring($directMixedReturnArrayInterpStart, $directMixedReturnArrayInterpIndex - $directMixedReturnArrayInterpStart)
    if (([regex]::Matches($directMixedReturnArrayInterpWindow, 'call \$make_mixed_values_ast').Count -ne 1) -or
        $directMixedReturnArrayInterpWindow -notmatch '(?s)call \$make_mixed_values_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp' -or
        ([regex]::Matches($directMixedReturnArrayInterpWindow, 'local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_array_get_checked').Count -lt 4) -or
        $directMixedReturnArrayInterpWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
        $directMixedReturnArrayInterpWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\)' -or
        $directMixedReturnArrayInterpWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 3\s+call \$__sura_array_get_checked\s+drop') {
        throw "direct mixed primitive array function returns should stringify bare interpolation with per-element conversions"
    }
    $directMixedReturnArrayConcatIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_array_concat_label')
    if ($directMixedReturnArrayConcatIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return array string-concat label lowering"
    }
    $directMixedReturnArrayConcatStart = $astWatText.LastIndexOf('call $make_mixed_values_ast', $directMixedReturnArrayConcatIndex)
    if ($directMixedReturnArrayConcatStart -lt 0) { $directMixedReturnArrayConcatStart = [Math]::Max(0, $directMixedReturnArrayConcatIndex - 28000) }
    $directMixedReturnArrayConcatWindow = $astWatText.Substring($directMixedReturnArrayConcatStart, $directMixedReturnArrayConcatIndex - $directMixedReturnArrayConcatStart)
    if (([regex]::Matches($directMixedReturnArrayConcatWindow, 'call \$make_mixed_values_ast').Count -ne 1) -or
        $directMixedReturnArrayConcatWindow -notmatch '(?s)call \$make_mixed_values_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp' -or
        ([regex]::Matches($directMixedReturnArrayConcatWindow, 'local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_array_get_checked').Count -lt 4) -or
        $directMixedReturnArrayConcatWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
        $directMixedReturnArrayConcatWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 2\s+call \$__sura_array_get_checked\s+if \(result i32\)' -or
        $directMixedReturnArrayConcatWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const 3\s+call \$__sura_array_get_checked\s+drop' -or
        $directMixedReturnArrayConcatWindow -notmatch 'call \$__sura_string_concat') {
        throw "direct mixed primitive array function returns should stringify string concatenation with per-element conversions"
    }
    $directMixedReturnCollectionRuntimeIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_collection_runtime_label')
    if ($directMixedReturnCollectionRuntimeIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return collection runtime label lowering"
    }
    $directMixedReturnCollectionRuntimeStart = [Math]::Max(0, $directMixedReturnCollectionRuntimeIndex - 30000)
    $directMixedReturnCollectionRuntimeWindow = $astWatText.Substring($directMixedReturnCollectionRuntimeStart, $directMixedReturnCollectionRuntimeIndex - $directMixedReturnCollectionRuntimeStart)
    if ($directMixedReturnCollectionRuntimeWindow -notmatch '(?s)call \$make_mixed_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)call \$make_mixed_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_is_truthy' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)call \$make_mixed_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_length' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)call \$make_mixed_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)i32\.const 0\s+call \$make_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)i32\.const 0\s+call \$make_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_is_truthy' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)i32\.const 0\s+call \$make_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_length' -or
        $directMixedReturnCollectionRuntimeWindow -notmatch '(?s)i32\.const 0\s+call \$make_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy') {
        throw "direct function-returned collection length/to_bool should lower through Value collection helpers"
    }
    $paramReturnCollectionRuntimeIndex = $astWatText.IndexOf('local.set $param_return_collection_runtime_label')
    if ($paramReturnCollectionRuntimeIndex -lt 0) {
        throw "generated AST JSON WASM should include parameter-returned collection runtime label lowering"
    }
    $paramReturnCollectionRuntimeStart = [Math]::Max(0, $paramReturnCollectionRuntimeIndex - 30000)
    $paramReturnCollectionRuntimeWindow = $astWatText.Substring($paramReturnCollectionRuntimeStart, $paramReturnCollectionRuntimeIndex - $paramReturnCollectionRuntimeStart)
    if ($paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$values\s+call \$pass_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$values\s+call \$pass_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_is_truthy' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$empty_values\s+call \$pass_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_length' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$empty_values\s+call \$pass_values_ast.*?call \$__sura_value_array.*?call \$__sura_value_is_truthy' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$meta\s+call \$pass_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_length' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$meta\s+call \$pass_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$empty_profile\s+call \$pass_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_length' -or
        $paramReturnCollectionRuntimeWindow -notmatch '(?s)local\.get \$empty_profile\s+call \$pass_profile_ast.*?call \$__sura_value_dict.*?call \$__sura_value_is_truthy') {
        throw "parameter-returned collection length/to_bool should preserve argument-derived array/dict Value hints"
    }
    foreach ($functionChoiceLabel in @("function_return_choice_collection_to_str_label", "function_return_local_choice_collection_to_str_label")) {
        $functionChoiceIndex = $astWatText.IndexOf("local.set `$$functionChoiceLabel")
        if ($functionChoiceIndex -lt 0) {
            throw "generated AST JSON WASM should include $functionChoiceLabel lowering"
        }
        $functionChoiceStart = [Math]::Max(0, $functionChoiceIndex - 36000)
        $functionChoiceWindow = $astWatText.Substring($functionChoiceStart, $functionChoiceIndex - $functionChoiceStart)
        if ($functionChoiceWindow -notmatch 'if \(result i32\)' -or
            $functionChoiceWindow -notmatch 'call \$__sura_array_to_string_num' -or
            $functionChoiceWindow -notmatch '(?s)call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
            $functionChoiceWindow -notmatch '(?s)i32\.const 123.*?i32\.const 125') {
            throw "$functionChoiceLabel should lower full-or-empty function-returned arrays/dicts branch-wise"
        }
    }
    foreach ($functionAccessLabel in @("function_return_choice_access_label", "function_return_local_choice_access_label")) {
        $functionAccessIndex = $astWatText.IndexOf("local.set `$$functionAccessLabel")
        if ($functionAccessIndex -lt 0) {
            throw "generated AST JSON WASM should include $functionAccessLabel lowering"
        }
        $functionAccessStart = [Math]::Max(0, $functionAccessIndex - 36000)
        $functionAccessWindow = $astWatText.Substring($functionAccessStart, $functionAccessIndex - $functionAccessStart)
        if ($functionAccessWindow -notmatch 'call \$choose_values' -or
            $functionAccessWindow -notmatch '(?s)call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
            $functionAccessWindow -notmatch 'call \$choose_profile' -or
            $functionAccessWindow -notmatch 'call \$__sura_dict_get') {
            throw "$functionAccessLabel should preserve exact index/key hints through conditional function-returned collections"
        }
    }
    $functionAliasCollectionIndex = $astWatText.IndexOf('local.set $function_return_alias_collection_to_str_label')
    if ($functionAliasCollectionIndex -lt 0) {
        throw "generated AST JSON WASM should include function_return_alias_collection_to_str_label lowering"
    }
    $functionAliasCollectionStart = [Math]::Max(0, $functionAliasCollectionIndex - 36000)
    $functionAliasCollectionWindow = $astWatText.Substring($functionAliasCollectionStart, $functionAliasCollectionIndex - $functionAliasCollectionStart)
    if ($functionAliasCollectionWindow -notmatch 'local\.get \$function_return_alias_values\s+call \$__sura_array_to_string_num' -or
        $functionAliasCollectionWindow -notmatch '(?s)i32\.const 91.*?i32\.const 93' -or
        $functionAliasCollectionWindow -notmatch '(?s)local\.get \$function_return_alias_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
        $functionAliasCollectionWindow -notmatch '(?s)i32\.const 123.*?i32\.const 125') {
        throw "function-returned collection aliases should preserve full, empty, and ordered dict hints for to_str(value)"
    }
    $functionAliasAccessIndex = $astWatText.IndexOf('local.set $function_return_alias_access_label')
    if ($functionAliasAccessIndex -lt 0) {
        throw "generated AST JSON WASM should include function_return_alias_access_label lowering"
    }
    $functionAliasAccessStart = [Math]::Max(0, $functionAliasAccessIndex - 24000)
    $functionAliasAccessWindow = $astWatText.Substring($functionAliasAccessStart, $functionAliasAccessIndex - $functionAliasAccessStart)
    if ($functionAliasAccessWindow -notmatch '(?s)local\.get \$function_return_alias_values\s+i32\.const 0\s+call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
        $functionAliasAccessWindow -notmatch '(?s)local\.get \$function_return_alias_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_type_name' -or
        $functionAliasAccessWindow -notmatch '(?s)local\.get \$function_return_alias_profile\s+local\.get \$collection_profile_key\s+call \$__sura_string_hash\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_to_string') {
        throw "function-returned collection aliases should preserve exact index/key hints for later access"
    }
    $functionLocalScopeAliasIndex = $astWatText.IndexOf('local.set $function_return_local_scope_alias_label')
    if ($functionLocalScopeAliasIndex -lt 0) {
        throw "generated AST JSON WASM should include function_return_local_scope_alias_label lowering"
    }
    $functionLocalScopeAliasStart = [Math]::Max(0, $functionLocalScopeAliasIndex - 16000)
    $functionLocalScopeAliasWindow = $astWatText.Substring($functionLocalScopeAliasStart, $functionLocalScopeAliasIndex - $functionLocalScopeAliasStart)
    if ($functionLocalScopeAliasWindow -notmatch 'call \$function_alias_values_text_ast' -or
        $functionLocalScopeAliasWindow -notmatch 'call \$function_alias_empty_values_text_ast' -or
        $functionLocalScopeAliasWindow -notmatch 'call \$function_alias_profile_text_ast' -or
        $functionLocalScopeAliasWindow -notmatch 'call \$function_alias_empty_profile_text_ast' -or
        $functionLocalScopeAliasWindow -notmatch 'call \$function_alias_profile_bonus_text_ast') {
        throw "function-local aliases should compile through call-site-observed collection-shaped parameters"
    }
    $constructorCollectionStart = $astWatText.IndexOf('(func $__sura_new_AstCollectionTagged')
    if ($constructorCollectionStart -lt 0) {
        throw "generated AST JSON WASM should include AstCollectionTagged constructor lowering"
    }
    $constructorCollectionNext = $astWatText.IndexOf("  (func ", $constructorCollectionStart + 1)
    if ($constructorCollectionNext -lt 0) { $constructorCollectionNext = $astWatText.Length }
    $constructorCollectionBody = $astWatText.Substring($constructorCollectionStart, $constructorCollectionNext - $constructorCollectionStart)
    if ($constructorCollectionBody -notmatch '(?s)local\.get \$items\s+local\.set \$ctor_items' -or
        $constructorCollectionBody -notmatch '(?s)local\.get \$ctor_items\s+call \$__sura_array_to_string_num' -or
        $constructorCollectionBody -notmatch '(?s)local\.get \$profile\s+local\.set \$ctor_profile' -or
        $constructorCollectionBody -notmatch '(?s)local\.get \$ctor_profile\s+i32\.const \d+\s+call \$__sura_dict_get') {
        throw "constructor-local aliases should compile through new-expression-observed collection-shaped parameters"
    }
    $superConstructorCollectionStart = $astWatText.IndexOf('(func $__sura_new_AstCollectionParent')
    if ($superConstructorCollectionStart -lt 0) {
        throw "generated AST JSON WASM should include AstCollectionParent constructor lowering"
    }
    $superConstructorCollectionNext = $astWatText.IndexOf("  (func ", $superConstructorCollectionStart + 1)
    if ($superConstructorCollectionNext -lt 0) { $superConstructorCollectionNext = $astWatText.Length }
    $superConstructorCollectionBody = $astWatText.Substring($superConstructorCollectionStart, $superConstructorCollectionNext - $superConstructorCollectionStart)
    if ($superConstructorCollectionBody -notmatch '(?s)local\.get \$items\s+local\.set \$super_items' -or
        $superConstructorCollectionBody -notmatch '(?s)local\.get \$super_items\s+call \$__sura_array_to_string_num' -or
        $superConstructorCollectionBody -notmatch '(?s)local\.get \$profile\s+local\.set \$super_profile' -or
        $superConstructorCollectionBody -notmatch '(?s)local\.get \$super_profile\s+i32\.const \d+\s+call \$__sura_dict_get') {
        throw "super.init parent constructor aliases should compile through observed collection-shaped parameters"
    }
    $superMethodCollectionStart = $astWatText.IndexOf('(func $__sura_method_AstCollectionParent_describe_parent')
    if ($superMethodCollectionStart -lt 0) {
        throw "generated AST JSON WASM should include AstCollectionParent describe_parent lowering"
    }
    $superMethodCollectionNext = $astWatText.IndexOf("  (func ", $superMethodCollectionStart + 1)
    if ($superMethodCollectionNext -lt 0) { $superMethodCollectionNext = $astWatText.Length }
    $superMethodCollectionBody = $astWatText.Substring($superMethodCollectionStart, $superMethodCollectionNext - $superMethodCollectionStart)
    if ($superMethodCollectionBody -notmatch '(?s)local\.get \$items\s+local\.set \$parent_items' -or
        $superMethodCollectionBody -notmatch '(?s)local\.get \$parent_items\s+call \$__sura_array_to_string_num' -or
        $superMethodCollectionBody -notmatch '(?s)local\.get \$profile\s+local\.set \$parent_profile' -or
        $superMethodCollectionBody -notmatch '(?s)local\.get \$parent_profile\s+i32\.const \d+\s+call \$__sura_dict_get') {
        throw "super.method parent method aliases should compile through observed collection-shaped parameters"
    }
    $superMethodReturnCollectionIndex = $astWatText.IndexOf('local.set $super_method_return_collection_label')
    if ($superMethodReturnCollectionIndex -lt 0) {
        throw "generated AST JSON WASM should include super_method_return_collection_label lowering"
    }
    $superMethodReturnCollectionStart = [Math]::Max(0, $superMethodReturnCollectionIndex - 32000)
    $superMethodReturnCollectionWindow = $astWatText.Substring($superMethodReturnCollectionStart, $superMethodReturnCollectionIndex - $superMethodReturnCollectionStart)
    $superMethodReturnArrayString = (
        $superMethodReturnCollectionWindow -match '(?s)local\.get \$values\s+call \$__sura_array_to_string_num' -or
        $superMethodReturnCollectionWindow -match '(?s)call \$__sura_method_AstCollectionChild_values_via_super\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_array_to_string_num'
    )
    $superMethodReturnDictRead = (
        $superMethodReturnCollectionWindow -match '(?s)(?:call \$__sura_method_AstCollectionChild_profile_via_super|local\.get \$meta)\s+(?:i32\.const \d+|local\.get \$collection_profile_key\s+call \$__sura_string_hash)\s+call \$__sura_dict_get' -or
        $superMethodReturnCollectionWindow -match '(?s)call \$__sura_method_AstCollectionChild_profile_via_super\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+(?:i32\.const \d+|local\.get \$collection_profile_key\s+call \$__sura_string_hash)\s+call \$__sura_dict_get' -or
        ($superMethodReturnCollectionWindow -match 'call \$__sura_method_AstCollectionChild_profile_via_super' -and
         $superMethodReturnCollectionWindow -match 'call \$__sura_value_(field|index)')
    )
    if (-not $superMethodReturnArrayString -or
        -not $superMethodReturnDictRead -or
        $superMethodReturnCollectionWindow -match '(?s)local\.get \$collection_profile_key\s+i32\.const 4\s+i32\.mul\s+i32\.add\s+i32\.load') {
        throw "super.method-returned collection values should preserve caller-side to_str and exact key lowering"
    }
    $superMethodUpdateIndex = $astWatText.IndexOf('local.set $super_method_update_label')
    if ($superMethodUpdateIndex -lt 0) {
        throw "generated AST JSON WASM should include super_method_update_label lowering"
    }
    $superMethodUpdateStart = $astWatText.LastIndexOf('local.set $super_update_meta', $superMethodUpdateIndex)
    if ($superMethodUpdateStart -lt 0) { $superMethodUpdateStart = [Math]::Max(0, $superMethodUpdateIndex - 14000) }
    $superMethodUpdateWindow = $astWatText.Substring($superMethodUpdateStart, $superMethodUpdateIndex - $superMethodUpdateStart)
    $superMethodUpdateWrite = (
        $superMethodUpdateWindow -match 'call \$__sura_dict_set' -or
        $superMethodUpdateWindow -match 'call \$__sura_dict_put' -or
        $superMethodUpdateWindow -match 'call \$__sura_value_set_index'
    )
    $superMethodUpdateRead = (
        $superMethodUpdateWindow -match 'call \$__sura_dict_get' -or
        $superMethodUpdateWindow -match 'call \$__sura_value_index'
    )
    if ($superMethodUpdateWindow -notmatch 'local\.set \$super_update_meta' -or
        $superMethodUpdateWindow -notmatch 'local\.set \$super_method_update_profile' -or
        -not $superMethodUpdateWrite -or
        -not $superMethodUpdateRead -or
        $superMethodUpdateWindow -match '(?s)local\.get \$super_method_update_profile\s+local\.get \$collection_profile_key\s+i32\.const 4\s+i32\.mul\s+i32\.add\s+i32\.const 11\s+i32\.store') {
        throw "super.method-returned dict aliases with known string-key variables should update through dict_set, not array-store fallback"
    }
    $superMethodDynamicUpdateIndex = $astWatText.IndexOf('local.set $super_method_dynamic_update_label')
    if ($superMethodDynamicUpdateIndex -lt 0) {
        throw "generated AST JSON WASM should include super_method_dynamic_update_label lowering"
    }
    $superMethodDynamicUpdateStart = $astWatText.LastIndexOf('local.set $super_dynamic_update_meta', $superMethodDynamicUpdateIndex)
    if ($superMethodDynamicUpdateStart -lt 0) { $superMethodDynamicUpdateStart = [Math]::Max(0, $superMethodDynamicUpdateIndex - 16000) }
    $superMethodDynamicUpdateWindow = $astWatText.Substring($superMethodDynamicUpdateStart, $superMethodDynamicUpdateIndex - $superMethodDynamicUpdateStart)
    $superMethodDynamicUpdateWrite = (
        $superMethodDynamicUpdateWindow -match 'call \$__sura_dict_set' -or
        $superMethodDynamicUpdateWindow -match 'call \$__sura_dict_put' -or
        $superMethodDynamicUpdateWindow -match 'call \$__sura_value_set_index'
    )
    $superMethodDynamicUpdateRead = (
        $superMethodDynamicUpdateWindow -match 'call \$__sura_dict_get' -or
        $superMethodDynamicUpdateWindow -match 'call \$__sura_value_index'
    )
    if ($superMethodDynamicUpdateWindow -notmatch 'local\.set \$super_dynamic_update_meta' -or
        $superMethodDynamicUpdateWindow -notmatch '(?s)call \$choose_collection_profile_key_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$super_method_dynamic_update_key' -or
        $superMethodDynamicUpdateWindow -notmatch 'local\.set \$super_method_dynamic_update_profile' -or
        -not $superMethodDynamicUpdateWrite -or
        -not $superMethodDynamicUpdateRead -or
        $superMethodDynamicUpdateWindow -match '(?s)local\.get \$super_method_dynamic_update_profile\s+local\.get \$super_method_dynamic_update_key\s+i32\.const 4\s+i32\.mul') {
        throw "super.method-returned dict aliases with dynamic string-typed key variables should use string hashing for dict read/write"
    }
    $superCallKeyStringIndex = $astWatText.IndexOf('local.set $super_call_key_string_label')
    if ($superCallKeyStringIndex -lt 0) {
        throw "generated AST JSON WASM should include super_call_key_string_label lowering"
    }
    $superCallKeyStringStart = [Math]::Max(0, $superCallKeyStringIndex - 16000)
    $superCallKeyStringWindow = $astWatText.Substring($superCallKeyStringStart, $superCallKeyStringIndex - $superCallKeyStringStart)
    if ($superCallKeyStringWindow -notmatch 'local\.set \$super_call_key_string_profile' -or
        $superCallKeyStringWindow -notmatch '(?s)local\.get \$super_call_key_string_profile.*?i32\.const \d+.*?call \$__sura_dict_put\s+local\.set \$super_call_key_string_profile' -or
        $superCallKeyStringWindow -notmatch '(?s)local\.get \$super_call_key_string_profile.*?i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_string_concat' -or
        $superCallKeyStringWindow -match '(?s)local\.get \$super_call_key_string_profile.*?call \$__sura_i32_to_string\s+call \$__sura_string_concat\s+local\.set \$super_call_key_string_label') {
        throw "function-call-derived string keys on super.method-returned dict aliases should preserve string access hints for concatenation"
    }
    $superMethodKeyStringIndex = $astWatText.IndexOf('local.set $super_method_key_string_label')
    if ($superMethodKeyStringIndex -lt 0) {
        throw "generated AST JSON WASM should include super_method_key_string_label lowering"
    }
    $superMethodKeyStringStart = [Math]::Max(0, $superMethodKeyStringIndex - 16000)
    $superMethodKeyStringWindow = $astWatText.Substring($superMethodKeyStringStart, $superMethodKeyStringIndex - $superMethodKeyStringStart)
    if ($superMethodKeyStringWindow -notmatch 'local\.set \$super_method_key_string_profile' -or
        $superMethodKeyStringWindow -notmatch '(?s)local\.get \$super_method_key_string_profile.*?i32\.const \d+.*?call \$__sura_dict_put\s+local\.set \$super_method_key_string_profile' -or
        $superMethodKeyStringWindow -notmatch '(?s)local\.get \$super_method_key_string_profile.*?i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_string_concat' -or
        $superMethodKeyStringWindow -match '(?s)local\.get \$super_method_key_string_profile.*?call \$__sura_i32_to_string\s+call \$__sura_string_concat\s+local\.set \$super_method_key_string_label') {
        throw "method-call-derived string keys on super.method-returned dict aliases should resolve exact string keys for concatenation"
    }
    $methodIndexStringIndex = $astWatText.IndexOf('local.set $method_index_string_label')
    if ($methodIndexStringIndex -lt 0) {
        throw "generated AST JSON WASM should include method_index_string_label lowering"
    }
    $methodIndexStringStart = [Math]::Max(0, $methodIndexStringIndex - 12000)
    $methodIndexStringWindow = $astWatText.Substring($methodIndexStringStart, $methodIndexStringIndex - $methodIndexStringStart)
    if ($methodIndexStringWindow -notmatch 'local\.set \$method_index_string_items' -or
        $methodIndexStringWindow -notmatch '(?s)local\.get \$method_index_string_items.*?i32\.const 1.*?call \$__sura_array_set_checked' -or
        $methodIndexStringWindow -notmatch '(?s)local\.get \$method_index_string_items.*?i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $methodIndexStringWindow -match '(?s)call \$__sura_method_AstCollectionTagged_item_index\s+i32\.const 4\s+i32\.mul' -or
        $methodIndexStringWindow -match '(?s)local\.get \$method_index_string_items.*?call \$__sura_i32_to_string\s+call \$__sura_string_concat\s+local\.set \$method_index_string_label') {
        throw "method-call-derived numeric array indexes should resolve exact string element reads for concatenation"
    }
    $methodIndexValueIndex = $astWatText.IndexOf('local.set $method_index_value_label')
    if ($methodIndexValueIndex -lt 0) {
        throw "generated AST JSON WASM should include method_index_value_label lowering"
    }
    $methodIndexValueStart = [Math]::Max(0, $methodIndexValueIndex - 16000)
    $methodIndexValueWindow = $astWatText.Substring($methodIndexValueStart, $methodIndexValueIndex - $methodIndexValueStart)
    if ($methodIndexValueWindow -notmatch 'local\.set \$method_index_mixed_items' -or
        $methodIndexValueWindow -notmatch '(?s)local\.get \$method_index_mixed_items.*?i32\.const 1.*?call \$__sura_array_set_checked' -or
        ([regex]::Matches($methodIndexValueWindow, '(?s)local\.get \$method_index_mixed_items.*?i32\.const 1\s+call \$__sura_array_get_checked').Count -lt 3) -or
        $methodIndexValueWindow -notmatch '(?s)local\.get \$method_index_mixed_items.*?i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_string_concat' -or
        $methodIndexValueWindow -notmatch '(?s)local\.get \$method_index_mixed_items.*?i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_length' -or
        $methodIndexValueWindow -match '(?s)call \$__sura_method_AstCollectionTagged_item_index\s+i32\.const 4\s+i32\.mul') {
        throw "method-call-derived numeric indexes should feed exact array reads into type(), to_str(), and length()"
    }
    $methodIndexBoolIndex = $astWatText.IndexOf('local.set $method_index_bool_label')
    if ($methodIndexBoolIndex -lt 0) {
        throw "generated AST JSON WASM should include method_index_bool_label lowering"
    }
    $methodIndexBoolStart = [Math]::Max(0, $methodIndexBoolIndex - 12000)
    $methodIndexBoolWindow = $astWatText.Substring($methodIndexBoolStart, $methodIndexBoolIndex - $methodIndexBoolStart)
    if ($methodIndexBoolWindow -notmatch 'local\.set \$method_index_bool_items' -or
        $methodIndexBoolWindow -notmatch '(?s)local\.get \$method_index_bool_items.*?i32\.const 1.*?call \$__sura_array_set_checked' -or
        $methodIndexBoolWindow -notmatch '(?s)local\.get \$method_index_bool_items.*?i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_value_bool\s+call \$__sura_value_is_truthy' -or
        $methodIndexBoolWindow -match '(?s)call \$__sura_method_AstCollectionTagged_item_index\s+i32\.const 4\s+i32\.mul') {
        throw "method-call-derived numeric indexes should feed exact bool array reads into to_bool()"
    }
    $methodIndexObjectIndex = $astWatText.IndexOf('local.set $method_index_object_index_label')
    if ($methodIndexObjectIndex -lt 0) {
        throw "generated AST JSON WASM should include method-index object receiver label lowering"
    }
    $methodIndexObjectStart = [Math]::Max(0, $methodIndexObjectIndex - 18000)
    $methodIndexObjectWindow = $astWatText.Substring($methodIndexObjectStart, $methodIndexObjectIndex - $methodIndexObjectStart)
    if ($methodIndexObjectWindow -notmatch 'local\.set \$method_index_tagged_items' -or
        $methodIndexObjectWindow -notmatch '(?s)local\.get \$method_index_tagged_items.*?i32\.const 1\s+call \$__sura_new_AstTagged.*?call \$__sura_array_set_checked' -or
        $methodIndexObjectWindow -notmatch '(?s)local\.get \$method_index_tagged_items.*?i32\.const 1\s+call \$__sura_array_get_checked\s+call \$__sura_method_AstTagged_kind_text.*?local\.set \$method_index_object_label' -or
        ([regex]::Matches($methodIndexObjectWindow, '(?s)local\.get \$method_index_tagged_items.*?i32\.const 1\s+call \$__sura_array_get_checked\s+i32\.const \d+\s+call \$__sura_dict_get').Count -lt 2) -or
        $methodIndexObjectWindow -match '(?s)call \$__sura_method_AstCollectionTagged_item_index\s+i32\.const 4\s+i32\.mul') {
        throw "method-call-derived numeric indexes should preserve object receiver hints for method, field, and string-key access"
    }
    $methodReturnCollectionToStrIndex = $astWatText.IndexOf('local.set $method_return_collection_to_str_label')
    if ($methodReturnCollectionToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include method-returned collection to_str label lowering"
    }
    $methodReturnCollectionToStrStart = [Math]::Max(0, $methodReturnCollectionToStrIndex - 26000)
    $methodReturnCollectionToStrWindow = $astWatText.Substring($methodReturnCollectionToStrStart, $methodReturnCollectionToStrIndex - $methodReturnCollectionToStrStart)
    if ($methodReturnCollectionToStrWindow -notmatch '(?s)local\.get \$collection_tagged\s+call \$__sura_method_AstCollectionTagged_full_items_value\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_array_to_string_num' -or
        $methodReturnCollectionToStrWindow -notmatch '(?s)local\.get \$collection_tagged\s+call \$__sura_method_AstCollectionTagged_full_profile_value\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp.*?local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string') {
        throw "method-returned collection to_str should preserve constructor field array/dict hints through method return lowering"
    }
    foreach ($choiceCase in @(
        @{ label = "method_return_choice_collection_to_str_label"; items = "__sura_method_AstCollectionTagged_pick_items"; profile = "__sura_method_AstCollectionTagged_pick_profile" },
        @{ label = "method_return_local_choice_collection_to_str_label"; items = "__sura_method_AstCollectionTagged_pick_items_local"; profile = "__sura_method_AstCollectionTagged_pick_profile_local" }
    )) {
        $choiceLabel = [string]$choiceCase.label
        $itemsMethod = [string]$choiceCase.items
        $profileMethod = [string]$choiceCase.profile
        $choiceIndex = $astWatText.IndexOf("local.set `$$choiceLabel")
        if ($choiceIndex -lt 0) {
            throw "generated AST JSON WASM should include $choiceLabel lowering"
        }
        $choiceStart = [Math]::Max(0, $choiceIndex - 36000)
        $choiceWindow = $astWatText.Substring($choiceStart, $choiceIndex - $choiceStart)
        $itemsMethodPattern = [regex]::Escape("call `$$itemsMethod")
        $profileMethodPattern = [regex]::Escape("call `$$profileMethod")
        $itemsTrueStringifyPattern = '(?s)i32\.const 1\s+' + $itemsMethodPattern + '\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_array_to_string_num'
        $itemsFalseStringifyPattern = '(?s)i32\.const 0\s+' + $itemsMethodPattern + '\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp.*?call \$__sura_value_array.*?call \$__sura_value_to_string'
        $profileTrueStringifyPattern = '(?s)i32\.const 1\s+' + $profileMethodPattern + '\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp.*?call \$__sura_dict_get\s+call \$__sura_i32_to_string'
        $profileFalseStringifyPattern = '(?s)i32\.const 0\s+' + $profileMethodPattern + '\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp.*?call \$__sura_value_dict.*?call \$__sura_value_to_string'
        if ($choiceWindow -notmatch $itemsTrueStringifyPattern -or
            $choiceWindow -notmatch $itemsFalseStringifyPattern -or
            $choiceWindow -notmatch $profileTrueStringifyPattern -or
            $choiceWindow -notmatch $profileFalseStringifyPattern -or
            $choiceWindow -notmatch 'call \$__sura_array_to_string_num' -or
            $choiceWindow -notmatch '(?s)call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
            $choiceWindow -notmatch '(?s)i32\.const 123.*?i32\.const 125') {
            throw "$choiceLabel should lower full-or-empty method-returned arrays/dicts branch-wise"
        }
        foreach ($choiceMethod in @($itemsMethod, $profileMethod)) {
            $choiceMethodIndex = $astWatText.IndexOf("(func `$$choiceMethod")
            if ($choiceMethodIndex -lt 0) {
                throw "generated AST JSON WASM should include $choiceMethod lowering"
            }
            $choiceMethodEnd = $astWatText.IndexOf("  (func `$", $choiceMethodIndex + 10)
            if ($choiceMethodEnd -lt 0) { $choiceMethodEnd = [Math]::Min($astWatText.Length, $choiceMethodIndex + 12000) }
            $choiceMethodWindow = $astWatText.Substring($choiceMethodIndex, $choiceMethodEnd - $choiceMethodIndex)
            if ($choiceMethodWindow -notmatch 'if \(result i32\)' -or
                ([regex]::Matches($choiceMethodWindow, 'call \$__sura_dict_get').Count -lt 2)) {
                throw "$choiceMethod should keep branch-wise full-or-empty collection return lowering"
            }
        }
    }
    foreach ($accessLabel in @("method_return_choice_access_label", "method_return_local_choice_access_label")) {
        $accessIndex = $astWatText.IndexOf("local.set `$$accessLabel")
        if ($accessIndex -lt 0) {
            throw "generated AST JSON WASM should include $accessLabel lowering"
        }
        $accessStart = [Math]::Max(0, $accessIndex - 36000)
        $accessWindow = $astWatText.Substring($accessStart, $accessIndex - $accessStart)
        if ($accessWindow -notmatch 'call \$__sura_method_AstCollectionTagged_pick_items' -or
            $accessWindow -notmatch '(?s)call \$__sura_array_get_checked\s+call \$__sura_i32_to_string' -or
            $accessWindow -notmatch 'call \$__sura_method_AstCollectionTagged_pick_profile' -or
            $accessWindow -notmatch 'call \$__sura_dict_get') {
            throw "$accessLabel should preserve exact index/key hints through conditional method-returned collections"
        }
    }
    $directMixedReturnDictIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_dict_label')
    if ($directMixedReturnDictIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return dict label lowering"
    }
    $directMixedReturnDictStart = [Math]::Max(0, $directMixedReturnDictIndex - 24000)
    $directMixedReturnDictWindow = $astWatText.Substring($directMixedReturnDictStart, $directMixedReturnDictIndex - $directMixedReturnDictStart)
    if ($directMixedReturnDictWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_type_name' -or
        $directMixedReturnDictWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_to_string' -or
        $directMixedReturnDictWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_to_string' -or
        $directMixedReturnDictWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop') {
        throw "direct mixed primitive dict function returns should propagate fixed string-key conversions"
    }
    $directMixedReturnDictToStrIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_dict_to_str_label')
    if ($directMixedReturnDictToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return dict to_str label lowering"
    }
    $directMixedReturnDictToStrStart = $astWatText.LastIndexOf('call $make_mixed_profile_ast', $directMixedReturnDictToStrIndex)
    if ($directMixedReturnDictToStrStart -lt 0) { $directMixedReturnDictToStrStart = [Math]::Max(0, $directMixedReturnDictToStrIndex - 28000) }
    $directMixedReturnDictToStrWindow = $astWatText.Substring($directMixedReturnDictToStrStart, $directMixedReturnDictToStrIndex - $directMixedReturnDictToStrStart)
    if (([regex]::Matches($directMixedReturnDictToStrWindow, 'call \$make_mixed_profile_ast').Count -ne 1) -or
        $directMixedReturnDictToStrWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp' -or
        ([regex]::Matches($directMixedReturnDictToStrWindow, 'local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get').Count -lt 4) -or
        $directMixedReturnDictToStrWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
        $directMixedReturnDictToStrWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+if \(result i32\)' -or
        $directMixedReturnDictToStrWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop') {
        throw "direct mixed primitive dict function returns should stringify fixed keys with per-entry conversions"
    }
    $directMixedReturnDictInterpIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_dict_interp_label')
    if ($directMixedReturnDictInterpIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return dict interpolation label lowering"
    }
    $directMixedReturnDictInterpStart = $astWatText.LastIndexOf('call $make_mixed_profile_ast', $directMixedReturnDictInterpIndex)
    if ($directMixedReturnDictInterpStart -lt 0) { $directMixedReturnDictInterpStart = [Math]::Max(0, $directMixedReturnDictInterpIndex - 28000) }
    $directMixedReturnDictInterpWindow = $astWatText.Substring($directMixedReturnDictInterpStart, $directMixedReturnDictInterpIndex - $directMixedReturnDictInterpStart)
    if (([regex]::Matches($directMixedReturnDictInterpWindow, 'call \$make_mixed_profile_ast').Count -ne 1) -or
        $directMixedReturnDictInterpWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp' -or
        ([regex]::Matches($directMixedReturnDictInterpWindow, 'local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get').Count -lt 4) -or
        $directMixedReturnDictInterpWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
        $directMixedReturnDictInterpWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+if \(result i32\)' -or
        $directMixedReturnDictInterpWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop') {
        throw "direct mixed primitive dict function returns should stringify bare interpolation with per-entry conversions"
    }
    $directMixedReturnDictConcatIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_dict_concat_label')
    if ($directMixedReturnDictConcatIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return dict string-concat label lowering"
    }
    $directMixedReturnDictConcatStart = $astWatText.LastIndexOf('call $make_mixed_profile_ast', $directMixedReturnDictConcatIndex)
    if ($directMixedReturnDictConcatStart -lt 0) { $directMixedReturnDictConcatStart = [Math]::Max(0, $directMixedReturnDictConcatIndex - 28000) }
    $directMixedReturnDictConcatWindow = $astWatText.Substring($directMixedReturnDictConcatStart, $directMixedReturnDictConcatIndex - $directMixedReturnDictConcatStart)
    if (([regex]::Matches($directMixedReturnDictConcatWindow, 'call \$make_mixed_profile_ast').Count -ne 1) -or
        $directMixedReturnDictConcatWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+local\.set \$__sura_wasm_value_tmp' -or
        ([regex]::Matches($directMixedReturnDictConcatWindow, 'local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get').Count -lt 4) -or
        $directMixedReturnDictConcatWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
        $directMixedReturnDictConcatWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+if \(result i32\)' -or
        $directMixedReturnDictConcatWindow -notmatch '(?s)local\.get \$__sura_wasm_value_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop' -or
        $directMixedReturnDictConcatWindow -notmatch 'call \$__sura_string_concat') {
        throw "direct mixed primitive dict function returns should stringify string concatenation with per-entry conversions"
    }
    $directMixedReturnDotIndex = $astWatText.IndexOf('local.set $direct_mixed_function_return_dot_label')
    if ($directMixedReturnDotIndex -lt 0) {
        throw "generated AST JSON WASM should include direct mixed function-return dot label lowering"
    }
    $directMixedReturnDotStart = [Math]::Max(0, $directMixedReturnDotIndex - 24000)
    $directMixedReturnDotWindow = $astWatText.Substring($directMixedReturnDotStart, $directMixedReturnDotIndex - $directMixedReturnDotStart)
    if ($directMixedReturnDotWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_num\s+call \$__sura_value_type_name' -or
        $directMixedReturnDotWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_string_or_nil\s+call \$__sura_value_to_string' -or
        $directMixedReturnDotWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_value_bool\s+call \$__sura_value_to_string' -or
        $directMixedReturnDotWindow -notmatch '(?s)call \$make_mixed_profile_ast\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop') {
        throw "direct mixed primitive dict dot function returns should propagate fixed field conversions"
    }
    $dictVarToStrIndex = $astWatText.IndexOf('local.set $dict_var_to_str_label')
    if ($dictVarToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include mixed primitive dict variable to_str label lowering"
    }
    $dictVarToStrStart = [Math]::Max(0, $dictVarToStrIndex - 18000)
    $dictVarToStrWindow = $astWatText.Substring($dictVarToStrStart, $dictVarToStrIndex - $dictVarToStrStart)
    if (([regex]::Matches($dictVarToStrWindow, 'local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get').Count -lt 4) -or
        $dictVarToStrWindow -notmatch '(?s)local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
        $dictVarToStrWindow -notmatch '(?s)local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+if \(result i32\)' -or
        $dictVarToStrWindow -notmatch '(?s)local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop') {
        throw "mixed primitive dict variable to_str should lower fixed keys with per-entry conversions"
    }
    $nestedCollectionToStrIndex = $astWatText.IndexOf('local.set $nested_collection_to_str_label')
    if ($nestedCollectionToStrIndex -lt 0) {
        throw "generated AST JSON WASM should include nested collection variable to_str label lowering"
    }
    $nestedCollectionToStrStart = [Math]::Max(0, $nestedCollectionToStrIndex - 24000)
    $nestedCollectionToStrWindow = $astWatText.Substring($nestedCollectionToStrStart, $nestedCollectionToStrIndex - $nestedCollectionToStrStart)
    if ($nestedCollectionToStrWindow -notmatch '(?s)local\.get \$names\s+call \$__sura_array_to_string_string' -or
        $nestedCollectionToStrWindow -notmatch '(?s)local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+call \$__sura_i32_to_string' -or
        $nestedCollectionToStrWindow -notmatch '(?s)local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+if \(result i32\)' -or
        $nestedCollectionToStrWindow -notmatch '(?s)local\.get \$mixed_profile\s+i32\.const \d+\s+call \$__sura_dict_get\s+drop') {
        throw "nested collection to_str literals should reuse array and dict variable stringification"
    }
    $toStrMixedIndex = $astWatText.IndexOf('local.set $to_str_mixed_ternary_label')
    if ($toStrMixedIndex -lt 0) {
        throw "generated AST JSON WASM should include branch-sensitive mixed ternary to_str() label lowering"
    }
    $toStrMixedStart = [Math]::Max(0, $toStrMixedIndex - 20000)
    $toStrMixedWindow = $astWatText.Substring($toStrMixedStart, $toStrMixedIndex - $toStrMixedStart)
    if (([regex]::Matches($toStrMixedWindow, 'if \(result i32\)').Count -lt 4) -or
        $toStrMixedWindow -notmatch '(?s)i32\.const 121\s+i32\.const 101\s+i32\.const 115\s+call \$__sura_make_array_3\s+call \$__sura_value_string_or_nil' -or
        $toStrMixedWindow -notmatch 'call \$__sura_value_dynamic_array' -or
        $toStrMixedWindow -notmatch 'call \$__sura_value_dynamic_dict' -or
        $toStrMixedWindow -notmatch 'call \$__sura_value_to_string') {
        throw "mixed ternary to_str() should lower selected branches through Value string semantics"
    }
    $typeMixedIndex = $astWatText.IndexOf('local.set $type_mixed_ternary_label')
    if ($typeMixedIndex -lt 0) {
        throw "generated AST JSON WASM should include branch-sensitive mixed ternary type() label lowering"
    }
    $typeMixedStart = [Math]::Max(0, $typeMixedIndex - 16000)
    $typeMixedWindow = $astWatText.Substring($typeMixedStart, $typeMixedIndex - $typeMixedStart)
    if (([regex]::Matches($typeMixedWindow, 'if \(result i32\)').Count -lt 4) -or
        ([regex]::Matches($typeMixedWindow, 'call \$__sura_value_type_name').Count -lt 6) -or
        $typeMixedWindow -notmatch '(?s)call \$__sura_value_dynamic_array\s+call \$__sura_value_type_name.*?call \$__sura_value_dynamic_dict\s+call \$__sura_value_type_name' -or
        $typeMixedWindow -notmatch '(?s)call \$__sura_value_string_or_nil\s+call \$__sura_value_type_name.*?call \$__sura_value_nil\s+call \$__sura_value_type_name') {
        throw "mixed ternary type() should lower selected branch type names instead of a single static fallback"
    }
    $lenMixedIndex = $astWatText.IndexOf('local.set $len_mixed_ternary_label')
    if ($lenMixedIndex -lt 0) {
        throw "generated AST JSON WASM should include branch-sensitive mixed ternary length() label lowering"
    }
    $lenMixedStart = [Math]::Max(0, $lenMixedIndex - 16000)
    $lenMixedWindow = $astWatText.Substring($lenMixedStart, $lenMixedIndex - $lenMixedStart)
    if (([regex]::Matches($lenMixedWindow, 'if \(result i32\)').Count -lt 4) -or
        ([regex]::Matches($lenMixedWindow, 'call \$__sura_value_length')).Count -lt 4) {
        throw "mixed ternary length() should lower selected branch lengths instead of a single static type fallback"
    }
    $mixedCatchIndex = $astWatText.LastIndexOf('local.set $try_mixed_payload_after')
    if ($mixedCatchIndex -lt 0) {
        throw "generated AST JSON WASM should include try_mixed_payload_after"
    }
    $mixedCatchStart = $astWatText.LastIndexOf('local.set $err', $mixedCatchIndex)
    if ($mixedCatchStart -lt 0) { $mixedCatchStart = [Math]::Max(0, $mixedCatchIndex - 360) }
    $mixedCatchWindow = $astWatText.Substring($mixedCatchStart, $mixedCatchIndex - $mixedCatchStart)
    if ($mixedCatchWindow -notmatch 'call \$__sura_value_add') {
        throw "mixed catch payload types should preserve runtime number-or-string addition through the Value ABI"
    }
    $conflictStart = $astWatText.IndexOf('(func $conflict_probe')
    if ($conflictStart -lt 0) {
        throw "generated AST JSON WASM should include conflict_probe"
    }
    $conflictNext = $astWatText.IndexOf("  (func ", $conflictStart + 1)
    if ($conflictNext -lt 0) { $conflictNext = $astWatText.Length }
    $conflictBody = $astWatText.Substring($conflictStart, $conflictNext - $conflictStart)
    if ($conflictBody -notmatch 'call \$__sura_value_add') {
        throw "mixed observed function argument types should use runtime number-or-string addition through the Value ABI"
    }
    $methodConflictStart = $astWatText.IndexOf('(func $__sura_method_AstPoint_shifted_conflict')
    if ($methodConflictStart -lt 0) {
        throw "generated AST JSON WASM should include AstPoint.shifted_conflict"
    }
    $methodConflictNext = $astWatText.IndexOf("  (func ", $methodConflictStart + 1)
    if ($methodConflictNext -lt 0) { $methodConflictNext = $astWatText.Length }
    $methodConflictBody = $astWatText.Substring($methodConflictStart, $methodConflictNext - $methodConflictStart)
    if ($methodConflictBody -notmatch 'call \$__sura_value_add') {
        throw "mixed observed method argument types should use runtime number-or-string addition through the Value ABI"
    }
    $fieldConflictStart = $astWatText.IndexOf('(func $__sura_method_AstFieldConflict_bump')
    if ($fieldConflictStart -lt 0) {
        throw "generated AST JSON WASM should include AstFieldConflict.bump"
    }
    $fieldConflictNext = $astWatText.IndexOf("  (func ", $fieldConflictStart + 1)
    if ($fieldConflictNext -lt 0) { $fieldConflictNext = $astWatText.Length }
    $fieldConflictBody = $astWatText.Substring($fieldConflictStart, $fieldConflictNext - $fieldConflictStart)
    if ($fieldConflictBody -notmatch 'call \$__sura_value_add') {
        throw "mixed observed constructor field types should keep the field tagged and use runtime number-or-string addition"
    }
    if ($astWatText -notmatch '(?s)call \$__sura_method_AstFunctionHolder_handler_type\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_value_string(?:_or_nil)?' -or
        $astWatText -notmatch '(?s)call \$__sura_method_AstFunctionHolder_handler_truth\s+local\.set \$__sura_wasm_call_tmp.*?local\.get \$__sura_wasm_call_tmp\s+call \$__sura_value_bool') {
        throw "method return inference should preserve type() strings and to_bool() booleans at call sites"
    }
    $node = Get-Command node -ErrorAction Stop
    $runner = Join-Path $temp "ast_wasm_main_smoke.js"
    [System.IO.File]::WriteAllText($runner, @'
const fs = require("fs");
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  const ptr = instance.exports.main();
  const view = new DataView(instance.exports.memory.buffer);
  if (!Number.isInteger(ptr) || ptr < 4 || ptr > view.byteLength) {
    throw new Error(`AST WASM main returned invalid Sura string pointer ${ptr}`);
  }
  const length = view.getInt32(ptr - 4, true);
  if (length < 0 || ptr + length * 4 > view.byteLength) {
    throw new Error(`AST WASM main returned invalid Sura string length ${length}`);
  }
  const codePoints = [];
  for (let i = 0; i < length; i += 1) codePoints.push(view.getInt32(ptr + i * 4, true));
  const got = String.fromCodePoint(...codePoints);
  if (got !== "16") throw new Error(`AST WASM main expected output "16", got ${JSON.stringify(got)}`);
}).catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
'@, [System.Text.Encoding]::UTF8)
    & $node.Source $runner $astWasm
    if ($LASTEXITCODE -ne 0) {
        throw "AST JSON WASM main execution failed"
    }

    $unsupportedCases = @(
        @{ name = "input"; source = "input ""name""" },
        @{ name = "exit"; source = "exit 1" },
        @{ name = "random"; source = "value is random_int(10)" },
        @{ name = "console_status"; source = "value is console.status()" }
    )
    foreach ($case in $unsupportedCases) {
        $unsupportedSource = Join-Path $temp ("unsupported_" + $case.name + ".sura")
        $unsupportedWat = Join-Path $temp ("unsupported_" + $case.name + ".wat")
        [System.IO.File]::WriteAllText($unsupportedSource, [string]$case.source, [System.Text.Encoding]::UTF8)
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $unsupportedOut = & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $unsupportedSource -Out $unsupportedWat 2>&1 | Out-String
            $unsupportedCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPreference
        }
        if ($unsupportedCode -eq 0 -or $unsupportedOut -notmatch "explicitly rejects host-interactive") {
            Write-Output $unsupportedOut
            throw "WASM target should explicitly reject unsupported $($case.name) command builtins"
        }
    }

    $runner = Join-Path $temp "wasm_value_selftest.js"
    [System.IO.File]::WriteAllText($runner, @'
const fs = require("fs");
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  const got = instance.exports.__sura_value_runtime_selftest();
  if (got !== 55) {
    console.error("value ABI selftest expected 55, got " + got);
    process.exit(1);
  }
}).catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
'@, [System.Text.Encoding]::UTF8)
    & $node.Source $runner $wasm
    if ($LASTEXITCODE -ne 0) {
        throw "WASM tagged Value ABI selftest failed"
    }

    "wasm_target_smoke: PASS"
}
finally {
    if ($env:SURA_KEEP_WASM_SMOKE_TEMP) {
        Write-Host "SURA_KEEP_WASM_SMOKE_TEMP: $temp"
    } else {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
