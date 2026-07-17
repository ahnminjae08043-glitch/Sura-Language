param(
    [string]$Transpiler = (Join-Path $PSScriptRoot "sura_to_wasm.ps1"),
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Resolve-SuraEngine {
    param([string]$Requested)
    if ($Requested -and (Test-Path -LiteralPath $Requested)) {
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $root = Split-Path -Parent $PSScriptRoot
    foreach ($candidate in @((Join-Path $root "SuraLanguage.exe"), (Join-Path $root "SuraLanguage"))) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "Sura engine not found"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_wasm_dispatch_" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $source = Join-Path $temp "dispatch.sura"
    $wat = Join-Path $temp "dispatch.wat"
    $wasm = Join-Path $temp "dispatch.wasm"
    $runner = Join-Path $temp "dispatch.js"
    [System.IO.File]::WriteAllText($source, @'
func sum_eight(a, b, c, d, e, f, g, h) do
  return a + b + c + d + e + f + g + h
end

func mul_add_eight(a, b, c, d, e, f, g, h) do
  return a * b + c + d + e + f + g + h
end

func sum_nine(a, b, c, d, e, f, g, h, i) do
  return a + b + c + d + e + f + g + h + i
end

func mul_add_nine(a, b, c, d, e, f, g, h, i) do
  return a * b + c + d + e + f + g + h + i
end

func sum_twelve(a, b, c, d, e, f, g, h, i, j, k, l) do
  return a + b + c + d + e + f + g + h + i + j + k + l
end

func mul_add_twelve(a, b, c, d, e, f, g, h, i, j, k, l) do
  return a * b + c + d + e + f + g + h + i + j + k + l
end

func sum_six(a, b, c, d, e, f) do
  return a + b + c + d + e + f
end

func mul_add_six(a, b, c, d, e, f) do
  return a * b + c + d + e + f
end

func sum_seven(a, b, c, d, e, f, g) do
  return a + b + c + d + e + f + g
end

func mul_add_seven(a, b, c, d, e, f, g) do
  return a * b + c + d + e + f + g
end

func call_dynamic_eight(flag, a, b, c, d, e, f, g, h) do
  handler is flag ? sum_eight : mul_add_eight
  return handler(a, b, c, d, e, f, g, h)
end

func call_dynamic_nine(flag, a, b, c, d, e, f, g, h, i) do
  handler is flag ? sum_nine : mul_add_nine
  return handler(a, b, c, d, e, f, g, h, i)
end

func call_dynamic_twelve(flag, a, b, c, d, e, f, g, h, i, j, k, l) do
  handler is flag ? sum_twelve : mul_add_twelve
  return handler(a, b, c, d, e, f, g, h, i, j, k, l)
end


func call_dynamic_six(flag, a, b, c, d, e, f) do
  handler is flag ? sum_six : mul_add_six
  return handler(a, b, c, d, e, f)
end

func call_dynamic_seven(flag, a, b, c, d, e, f, g) do
  handler is flag ? sum_seven : mul_add_seven
  return handler(a, b, c, d, e, f, g)
end

assert_eq(call_dynamic_six(true, 1, 2, 3, 4, 5, 6), 21)
assert_eq(call_dynamic_six(false, 1, 2, 3, 4, 5, 6), 20)
assert_eq(call_dynamic_seven(true, 1, 2, 3, 4, 5, 6, 7), 28)
assert_eq(call_dynamic_seven(false, 1, 2, 3, 4, 5, 6, 7), 27)
assert_eq(call_dynamic_eight(true, 1, 2, 3, 4, 5, 6, 7, 8), 36)
assert_eq(call_dynamic_eight(false, 1, 2, 3, 4, 5, 6, 7, 8), 35)
assert_eq(call_dynamic_nine(true, 1, 2, 3, 4, 5, 6, 7, 8, 9), 45)
assert_eq(call_dynamic_nine(false, 1, 2, 3, 4, 5, 6, 7, 8, 9), 44)
assert_eq(call_dynamic_twelve(true, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), 78)
assert_eq(call_dynamic_twelve(false, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), 77)
'@, (New-Object System.Text.UTF8Encoding($false)))

    $enginePath = Resolve-SuraEngine $Engine
    $pwsh = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
    if (-not $pwsh) { $pwsh = (Get-Command powershell -ErrorAction Stop).Source }
    & $pwsh -NoProfile -ExecutionPolicy Bypass -File $Transpiler `
        -Source $source -Out $wat -WasmOut $wasm -Engine $enginePath | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wasm)) {
        throw "arbitrary observed-arity WASM fixture did not produce a binary"
    }

    $watText = [System.IO.File]::ReadAllText($wat, [System.Text.Encoding]::ASCII)
    if ($watText -notmatch '(?s)\(func \$__sura_call_function_6 .*?call \$mul_add_six.*?call \$sum_six' -or
        $watText -notmatch '(?s)\(func \$__sura_call_function_7 .*?call \$mul_add_seven.*?call \$sum_seven' -or
        $watText -notmatch '(?s)\(func \$__sura_call_function_8 .*?call \$mul_add_eight.*?call \$sum_eight' -or
        $watText -notmatch '(?s)\(func \$__sura_call_function_9 .*?call \$mul_add_nine.*?call \$sum_nine' -or
        $watText -notmatch '(?s)\(func \$__sura_call_function_12 .*?call \$mul_add_twelve.*?call \$sum_twelve' -or
        $watText -notmatch '(?s)\(func \$call_dynamic_six .*?call \$__sura_call_function_6' -or
        $watText -notmatch '(?s)\(func \$call_dynamic_seven .*?call \$__sura_call_function_7' -or
        $watText -notmatch '(?s)\(func \$call_dynamic_eight .*?call \$__sura_call_function_8' -or
        $watText -notmatch '(?s)\(func \$call_dynamic_nine .*?call \$__sura_call_function_9' -or
        $watText -notmatch '(?s)\(func \$call_dynamic_twelve .*?call \$__sura_call_function_12') {
        throw "module-observed function Value call arities did not use generated dispatchers"
    }

    [System.IO.File]::WriteAllText($runner, @'
const fs = require("fs");
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  const call = instance.exports.call_dynamic_eight;
  const callSix = instance.exports.call_dynamic_six;
  const callSeven = instance.exports.call_dynamic_seven;
  const callNine = instance.exports.call_dynamic_nine;
  const callTwelve = instance.exports.call_dynamic_twelve;
  const six = [callSix(1, 1, 2, 3, 4, 5, 6), callSix(0, 1, 2, 3, 4, 5, 6)];
  const seven = [callSeven(1, 1, 2, 3, 4, 5, 6, 7), callSeven(0, 1, 2, 3, 4, 5, 6, 7)];
  const first = call(1, 1, 2, 3, 4, 5, 6, 7, 8);
  const second = call(0, 1, 2, 3, 4, 5, 6, 7, 8);
  const nine = [callNine(1, 1, 2, 3, 4, 5, 6, 7, 8, 9), callNine(0, 1, 2, 3, 4, 5, 6, 7, 8, 9)];
  const twelve = [
    callTwelve(1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12),
    callTwelve(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12),
  ];
  if (six[0] !== 21 || six[1] !== 20 || seven[0] !== 28 || seven[1] !== 27 || first !== 36 || second !== 35 ||
      nine[0] !== 45 || nine[1] !== 44 || twelve[0] !== 78 || twelve[1] !== 77) {
    throw new Error(`unexpected dispatcher results: ${six}/${seven}/${first}/${second}/${nine}/${twelve}`);
  }
  console.log("wasm_function_dispatch: PASS (6-12 observed arities, runtime-selected targets)");
}).catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
'@, (New-Object System.Text.UTF8Encoding($false)))
    $node = (Get-Command node -ErrorAction Stop).Source
    & $node $runner $wasm
    if ($LASTEXITCODE -ne 0) { throw "module-observed-arity WASM runtime dispatch failed" }
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
