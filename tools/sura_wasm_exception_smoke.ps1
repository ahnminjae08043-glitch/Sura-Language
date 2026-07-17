param(
    [string]$Transpiler = (Join-Path $PSScriptRoot "sura_to_wasm.ps1"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "tests/wasm_exception_propagation.sura"),
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

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_wasm_exception_" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    $enginePath = Resolve-SuraEngine $Engine
    $nativeOutput = (& $enginePath $Source | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $nativeOutput -ne "73") {
        throw "native exception fixture expected output 73, got '$nativeOutput'"
    }

    $wat = Join-Path $temp "exception.wat"
    $wasm = Join-Path $temp "exception.wasm"
    & $Transpiler -Source $Source -Out $wat -WasmOut $wasm | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wasm)) {
        throw "WASM exception fixture failed WAT validation or binary emission"
    }

    $watText = [IO.File]::ReadAllText($wat, [Text.Encoding]::ASCII)
    foreach ($needle in @(
        '(global $__sura_exception_value_tagged',
        'global.get $__sura_exception_thrown',
        'global.set $__sura_exception_value_tagged',
        'local.set $__try_value_tagged'
    )) {
        if (-not $watText.Contains($needle)) { throw "generated WAT missing exception evidence: $needle" }
    }

    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { throw "Node.js is required for WASM runtime execution" }
    $runner = Join-Path $temp "run.js"
    [IO.File]::WriteAllText($runner, @'
const fs = require("fs");
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({ instance }) => {
  const ptr = instance.exports.main();
  const view = new DataView(instance.exports.memory.buffer);
  if (!Number.isInteger(ptr) || ptr < 4 || ptr + 8 > view.byteLength) {
    throw new Error(`main returned invalid Sura string pointer ${ptr}`);
  }
  const length = view.getInt32(ptr - 4, true);
  if (length < 0 || ptr + length * 4 > view.byteLength) {
    throw new Error(`main returned invalid Sura string length ${length}`);
  }
  const codePoints = [];
  for (let i = 0; i < length; i += 1) codePoints.push(view.getInt32(ptr + i * 4, true));
  const text = String.fromCodePoint(...codePoints);
  if (text !== "73") throw new Error(`WASM output expected 73, got ${JSON.stringify(text)}`);
  console.log("wasm_exception_runtime: PASS");
}).catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
'@, [Text.Encoding]::UTF8)
    & $node.Source $runner $wasm
    if ($LASTEXITCODE -ne 0) { throw "WASM exception runtime execution failed" }

    "wasm_exception_smoke: PASS"
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
