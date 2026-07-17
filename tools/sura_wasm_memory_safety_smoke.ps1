param(
    [string]$Transpiler = (Join-Path $PSScriptRoot "sura_to_wasm.ps1"),
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot
if (-not $Engine) {
    foreach ($candidate in @((Join-Path $root "SuraLanguage.exe"), (Join-Path $root "SuraLanguage"))) {
        if (Test-Path -LiteralPath $candidate) { $Engine = (Resolve-Path -LiteralPath $candidate).Path; break }
    }
}
if (-not $Engine -or -not (Test-Path -LiteralPath $Engine)) { throw "Sura engine not found" }

$cases = @(
    @{ Name = "index"; Source = (Join-Path $root "tests/wasm_index_safety.sura"); Expected = "41"; Mode = "output" },
    @{ Name = "optional"; Source = (Join-Path $root "tests/wasm_optional_chain_safety.sura"); Expected = "74"; Mode = "output" },
    @{ Name = "optional_alias"; Source = (Join-Path $root "tests/wasm_optional_alias_safety.sura"); Expected = "Ada nil"; Mode = "output" },
    @{ Name = "container_counts"; Source = (Join-Path $root "tests/wasm_container_count_safety.sura"); Expected = "ababab"; Mode = "output" },
    @{ Name = "string_lines"; Source = (Join-Path $root "tests/wasm_string_lines_probe.sura"); Expected = "3"; Mode = "output" },
    @{ Name = "string_literal_method_interp"; Source = (Join-Path $root "tests/wasm_string_literal_method_interp_safety.sura"); Expected = "Literal sura core"; Mode = "output" },
    @{ Name = "inline_function_interp"; Source = (Join-Path $root "tests/wasm_inline_function_interp_safety.sura"); Expected = "DirectInlineFn function true <Func <lambda>>"; Mode = "output" },
    @{ Name = "captured_inline_return"; Source = (Join-Path $root "tests/wasm_captured_inline_return_safety.sura"); Expected = "SuraLang true 12"; Mode = "output" },
    @{ Name = "reference_nil"; Source = (Join-Path $root "tests/wasm_reference_nil_safety.sura"); Expected = "1"; Mode = "output" },
    @{ Name = "value_coalesce"; Source = (Join-Path $root "tests/wasm_value_coalesce_safety.sura"); Expected = "Fallback"; Mode = "output" },
    @{ Name = "value_return_alias"; Source = (Join-Path $root "tests/wasm_value_return_alias_safety.sura"); Expected = "Alias array 2 one"; Mode = "output" },
    @{ Name = "dict_insert"; Source = (Join-Path $root "tests/wasm_dict_insert_safety.sura"); Expected = "Grace 7 2"; Mode = "output" },
    @{ Name = "dict_dot_insert"; Source = (Join-Path $root "tests/wasm_dict_dot_insert_safety.sura"); Expected = "7 Ada"; Mode = "output" },
    @{ Name = "dict_dynamic_num"; Source = (Join-Path $root "tests/wasm_dict_dynamic_num_safety.sura"); Expected = "NumDict 16"; Mode = "output" },
    @{ Name = "dict_nonempty_insert"; Source = (Join-Path $root "tests/wasm_dict_nonempty_insert_safety.sura"); Expected = "NumDict 16 20 3"; Mode = "output" },
    @{ Name = "dict_path_nil"; Source = (Join-Path $root "tests/wasm_dict_path_nil_safety.sura"); Expected = "42 Ari nil"; Mode = "output" },
    @{ Name = "array_remove_nil"; Source = (Join-Path $root "tests/wasm_array_remove_nil_safety.sura"); Expected = "0 nil 1"; Mode = "output" },
    @{ Name = "dynamic_dict_has"; Source = (Join-Path $root "tests/wasm_dynamic_dict_has_safety.sura"); Expected = "true true"; Mode = "output" },
    @{ Name = "dynamic_string_function"; Source = (Join-Path $root "tests/wasm_dynamic_string_function_safety.sura"); Expected = "Ax Bx"; Mode = "output" },
    @{ Name = "dynamic_collection_function"; Source = (Join-Path $root "tests/wasm_dynamic_collection_function_safety.sura"); Expected = "a b"; Mode = "output" },
    @{ Name = "oob"; Source = (Join-Path $root "tests/wasm_index_oob.sura"); Expected = ""; Mode = "trap" },
    @{ Name = "growth"; Source = (Join-Path $root "tests/wasm_memory_growth.sura"); Expected = "301"; Mode = "growth" }
)

$temp = Join-Path ([IO.Path]::GetTempPath()) ("sura_wasm_memory_" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { throw "Node.js is required for WASM runtime execution" }
    $runner = Join-Path $temp "run.js"
    [IO.File]::WriteAllText($runner, @'
const fs = require("fs");
const mode = process.argv[3];
const expected = process.argv[4];
function decode(instance, ptr) {
  const view = new DataView(instance.exports.memory.buffer);
  if (!Number.isInteger(ptr) || ptr < 4 || ptr + 4 > view.byteLength) throw new Error(`invalid output pointer ${ptr}`);
  const length = view.getInt32(ptr - 4, true);
  if (length < 0 || ptr + length * 4 > view.byteLength) throw new Error(`invalid output length ${length}`);
  const codePoints = [];
  for (let i = 0; i < length; i += 1) codePoints.push(view.getInt32(ptr + i * 4, true));
  return String.fromCodePoint(...codePoints);
}
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  if (mode === "output") {
    const invalidValueExports = [
      "__sura_value_invalid_zero_tag_selftest",
      "__sura_value_invalid_zero_payload_selftest",
      "__sura_value_invalid_misaligned_tag_selftest",
      "__sura_value_invalid_heap_end_payload_selftest",
      "__sura_value_invalid_raw_tag_selftest",
      "__sura_value_corrupt_guard_selftest",
      "__sura_value_corrupt_tag_selftest",
    ];
    for (const name of invalidValueExports) {
      let caught;
      try { instance.exports[name](); } catch (error) { caught = error; }
      if (!(caught instanceof WebAssembly.RuntimeError)) {
        throw new Error(`${name} did not reject the invalid Value handle with a WASM trap`);
      }
    }
    if (instance.exports.__sura_payload_empty_selftest() !== 0) {
      throw new Error("valid empty string/array/dict payloads did not pass their exact heap boundary");
    }
    if (instance.exports.__sura_string_repeat_empty_selftest() !== 0) {
      throw new Error("repeating an empty string produced a non-empty or invalid payload");
    }
    if (instance.exports.__sura_dict_put_selftest() !== 3) {
      throw new Error("dictionary insertion did not preserve length, value, and key text");
    }
    const invalidPayloadExports = [
      "__sura_invalid_string_payload_low_selftest",
      "__sura_invalid_array_payload_low_selftest",
      "__sura_invalid_dict_payload_low_selftest",
      "__sura_invalid_string_payload_misaligned_selftest",
      "__sura_invalid_array_payload_past_heap_selftest",
      "__sura_invalid_string_negative_length_selftest",
      "__sura_invalid_string_huge_length_selftest",
      "__sura_invalid_array_huge_length_selftest",
      "__sura_invalid_dict_huge_length_selftest",
      "__sura_invalid_payload_alloc_selftest",
      "__sura_invalid_array_repeat_count_selftest",
      "__sura_invalid_string_repeat_count_selftest",
      "__sura_invalid_string_concat_null_selftest",
      "__sura_invalid_array_range_count_selftest",
      "__sura_invalid_value_array_element_selftest",
      "__sura_invalid_value_dict_key_selftest",
      "__sura_value_invalid_string_payload_selftest",
      "__sura_value_invalid_array_payload_selftest",
      "__sura_value_invalid_dict_payload_selftest",
      "__sura_invalid_flatten_inner_payload_selftest",
    ];
    for (const name of invalidPayloadExports) {
      let caught;
      try { instance.exports[name](); } catch (error) { caught = error; }
      if (!(caught instanceof WebAssembly.RuntimeError)) {
        throw new Error(`${name} did not reject the invalid container payload with a WASM trap`);
      }
    }
  }
  if (mode === "trap") {
    let trapped = false;
    try { instance.exports.main(); } catch (error) { trapped = error instanceof WebAssembly.RuntimeError; }
    if (!trapped) throw new Error("out-of-bounds access did not produce a WASM trap");
    return;
  }
  const ptr = instance.exports.main();
  const text = decode(instance, ptr);
  if (text !== expected) throw new Error(`expected output ${JSON.stringify(expected)}, got ${JSON.stringify(text)}`);
  if (mode === "growth" && instance.exports.memory.buffer.byteLength <= 65536) {
    throw new Error("allocator did not grow linear memory beyond the initial page");
  }
}).catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
'@, [Text.Encoding]::UTF8)

    foreach ($case in $cases) {
        if ($case.Mode -eq "trap") {
            $oldPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $nativeError = (& $Engine $case.Source 2>&1 | Out-String)
                $nativeExit = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $oldPreference
            }
            if ($nativeExit -eq 0 -or $nativeError -notmatch 'E202|out of range') {
                throw "native OOB fixture did not fail with an array bounds error"
            }
        } else {
            $nativeOutput = (& $Engine $case.Source | Out-String).Trim()
            if ($LASTEXITCODE -ne 0 -or $nativeOutput -ne $case.Expected) {
                throw "native $($case.Name) fixture expected '$($case.Expected)', got '$nativeOutput'"
            }
        }

        $wat = Join-Path $temp ($case.Name + ".wat")
        $wasm = Join-Path $temp ($case.Name + ".wasm")
        & $Transpiler -Source $case.Source -Out $wat -WasmOut $wasm | Out-Host
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wasm)) {
            throw "WAT validation/binary emission failed for $($case.Name)"
        }
        & $node.Source $runner $wasm $case.Mode $case.Expected
        if ($LASTEXITCODE -ne 0) { throw "WASM execution failed for $($case.Name)" }
    }

    $growthWat = [IO.File]::ReadAllText((Join-Path $temp "growth.wat"), [Text.Encoding]::ASCII)
    foreach ($needle in @("(memory (export `"memory`") 1 1024)", "memory.grow", "i32.const 67108864", "call `$__sura_array_get_checked", "call `$__sura_array_set_checked", "(func `$__sura_value_require", "i32.const 1398166102", "call `$__sura_value_require", "(func `$__sura_span_require", "(func `$__sura_payload_header_require", "(func `$__sura_payload_alloc_base", "call `$__sura_payload_alloc_base", "(func `$__sura_string_require", "(func `$__sura_array_require", "(func `$__sura_dict_require", "i32.div_u", "i64.extend_i32_s", "i64.extend_i32_u", "__sura_value_corrupt_guard_selftest", "__sura_value_corrupt_tag_selftest", "__sura_invalid_string_huge_length_selftest", "__sura_invalid_array_huge_length_selftest", "__sura_invalid_dict_huge_length_selftest", "__sura_invalid_array_repeat_count_selftest", "__sura_invalid_string_repeat_count_selftest", "__sura_invalid_string_concat_null_selftest", "__sura_invalid_array_range_count_selftest", "__sura_invalid_value_array_element_selftest", "__sura_invalid_value_dict_key_selftest")) {
        if (-not $growthWat.Contains($needle)) { throw "generated WAT missing safety evidence: $needle" }
    }
    $growthLines = [IO.File]::ReadAllLines((Join-Path $temp "growth.wat"), [Text.Encoding]::ASCII)
    $currentFunction = ""
    for ($lineIndex = 0; $lineIndex -lt $growthLines.Count; $lineIndex++) {
        $trimmed = $growthLines[$lineIndex].Trim()
        if ($growthLines[$lineIndex] -match '^\s*\(func \$(?<name>[^\s(]+)') {
            $currentFunction = [string]$Matches.name
        }
        if ($trimmed -ne 'call $__sura_alloc' -or $currentFunction -eq '__sura_payload_alloc_base') { continue }
        $previous = if ($lineIndex -gt 0) { $growthLines[$lineIndex - 1].Trim() } else { "" }
        if ($previous -notmatch '^i32\.const [1-9][0-9]*$') {
            throw "unsafe dynamic raw allocation remained in $currentFunction at WAT line $($lineIndex + 1)"
        }
    }
    "wasm_memory_safety_smoke: PASS"
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
