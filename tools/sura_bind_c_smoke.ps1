param(
    [string]$Surapkg = (Join-Path (Split-Path -Parent $PSScriptRoot) "surapkg.exe"),
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if (-not (Test-Path -LiteralPath $Surapkg)) {
    throw "surapkg not found: $Surapkg"
}
if (-not (Test-Path -LiteralPath $Engine)) {
    throw "Sura engine not found: $Engine"
}

$compiler = $null
foreach ($candidate in @("gcc", "cc", "g++")) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($cmd) {
        $compiler = $cmd.Source
        break
    }
}
if (-not $compiler) {
    throw "No C compiler found on PATH. Install gcc/cc/g++ to run the C FFI smoke test."
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_bind_c_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $header = Join-Path $temp "native.h"
    $out = Join-Path $temp "native.ffi.sura"
    $isWindows = $env:OS -eq "Windows_NT"
    $libName = if ($isWindows) { "native.dll" } elseif ($IsMacOS) { "libnative.dylib" } else { "libnative.so" }
    $lib = Join-Path $temp $libName
    $suraLib = $lib -replace "\\", "/"
    $source = Join-Path $temp "native.c"

    [System.IO.File]::WriteAllText(
        $header,
        @"
#ifndef NATIVE_H
#define NATIVE_H

#ifndef SURA_API
#define SURA_API
#endif

#define NATIVE_LIMIT 16
#define NATIVE_HEX_FLAG 0x10u
#define NATIVE_RATIO (2.5f)
#define NATIVE_NEGATIVE (-2)
#define NATIVE_NAME "sura-native"
#define NATIVE_PATH "lib\\native"
#define NATIVE_MACRO(x) ((x) + 1)

typedef int NativeCount;
typedef unsigned long NativeSizeAlias;
typedef const char* NativeText;
using NativeRatio = double;

typedef enum NativeStatus {
    NATIVE_STATUS_OK = 0,
    NATIVE_STATUS_BUSY,
    NATIVE_STATUS_ERROR = 0x10,
    NATIVE_STATUS_LAST
} NativeStatus;

enum NativeMode {
    NATIVE_MODE_FAST = -1,
    NATIVE_MODE_SAFE
};

#ifdef __cplusplus
extern "C" {
#endif

SURA_API int add(int left, int right) noexcept;
[[nodiscard]] double scale(double value, double factor) noexcept;
const char* version(void) noexcept;
void reset(void) noexcept;
const char* echo_text(const char* text = "fallback") noexcept;
int string_plus(const char* text, int extra = 0) noexcept;
std::size_t string_size(const char* text) noexcept;
double string_weight(const char* text, double weight) noexcept;
SURA_API NativeCount add_alias(NativeCount left, NativeCount right) noexcept;
NativeSizeAlias string_size_alias(NativeText text) noexcept;
NativeText echo_alias(NativeText text) noexcept;
NativeRatio scale_alias(NativeRatio value, NativeRatio factor) noexcept;
int too_many(int a, int b, int c, int d, int e) noexcept;
int needs_string(char const* text) noexcept;

class HiddenNative {
public:
    int should_not_bind(int value);
};

#ifdef __cplusplus
}
#endif

#endif
"@,
        $utf8NoBom
    )
    [System.IO.File]::WriteAllText(
        $source,
        @"
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

EXPORT int add(int left, int right) { return left + right; }
EXPORT double scale(double value, double factor) { return value * factor; }
EXPORT const char* version(void) { return "1.2.3"; }
EXPORT void reset(void) {}
EXPORT const char* echo_text(const char* text) { return text; }
EXPORT int string_plus(const char* text, int extra) { return (int)strlen(text) + extra; }
EXPORT size_t string_size(const char* text) { return strlen(text); }
EXPORT double string_weight(const char* text, double weight) { return strlen(text) * weight; }
EXPORT int add_alias(int left, int right) { return left + right + 10; }
EXPORT unsigned long string_size_alias(const char* text) { return (unsigned long)strlen(text); }
EXPORT const char* echo_alias(const char* text) { return text; }
EXPORT double scale_alias(double value, double factor) { return value * factor + 1.0; }
EXPORT int too_many(int a, int b, int c, int d, int e) { return a + b + c + d + e; }
EXPORT int needs_string(const char* text) { return (int)strlen(text); }

#ifdef __cplusplus
}
#endif
"@,
        $utf8NoBom
    )

    $compileArgs = @("-shared", "-O2", "-o", $lib, $source)
    if (-not $isWindows) {
        $compileArgs = @("-shared", "-fPIC", "-O2", "-o", $lib, $source)
    }
    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Native FFI test library compile failed with exit code $LASTEXITCODE"
    }

    $report = Join-Path $temp "bind-report.json"
    & $Surapkg bind-c $header --out $out --lib $suraLib --prefix native_ --json $report
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg bind-c failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $out)) {
        throw "expected generated binding file"
    }
    if (-not (Test-Path -LiteralPath $report)) {
        throw "expected generated bind-c JSON report"
    }
    $bindReport = Get-Content -Raw -Path $report | ConvertFrom-Json
    if ($bindReport.schema -ne "sura.bind_c.v1" -or
        $bindReport.passed -ne $true -or
        $bindReport.header -notmatch "native\.h" -or
        $bindReport.output -notmatch "native\.ffi\.sura" -or
        $bindReport.library -ne $suraLib -or
        $bindReport.prefix -ne "native_" -or
        $bindReport.emitted -ne 12 -or
        $bindReport.skipped -ne 2 -or
        $bindReport.emitted_constants -ne 12 -or
        @($bindReport.constants).Count -ne 12 -or
        @($bindReport.functions).Count -ne 14) {
        $bindReport | ConvertTo-Json -Depth 10 | Write-Output
        throw "expected bind-c JSON report to summarize generated wrappers"
    }
    $addReport = @($bindReport.functions) | Where-Object { $_.symbol -eq "add" } | Select-Object -First 1
    $skipReport = @($bindReport.functions) | Where-Object { $_.symbol -eq "too_many" } | Select-Object -First 1
    $mixedSkipReport = @($bindReport.functions) | Where-Object { $_.symbol -eq "string_weight" } | Select-Object -First 1
    $aliasAddReport = @($bindReport.functions) | Where-Object { $_.symbol -eq "add_alias" } | Select-Object -First 1
    $aliasTextReport = @($bindReport.functions) | Where-Object { $_.symbol -eq "echo_alias" } | Select-Object -First 1
    $aliasFloatReport = @($bindReport.functions) | Where-Object { $_.symbol -eq "scale_alias" } | Select-Object -First 1
    $limitConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_LIMIT" } | Select-Object -First 1
    $hexConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_HEX_FLAG" } | Select-Object -First 1
    $ratioConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_RATIO" } | Select-Object -First 1
    $negativeConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_NEGATIVE" } | Select-Object -First 1
    $nameConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_NAME" } | Select-Object -First 1
    $pathConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_PATH" } | Select-Object -First 1
    $macroConstant = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_MACRO" } | Select-Object -First 1
    $busyEnum = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_STATUS_BUSY" } | Select-Object -First 1
    $errorEnum = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_STATUS_ERROR" } | Select-Object -First 1
    $lastEnum = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_STATUS_LAST" } | Select-Object -First 1
    $safeEnum = @($bindReport.constants) | Where-Object { $_.symbol -eq "NATIVE_MODE_SAFE" } | Select-Object -First 1
    if (-not $addReport -or
        $addReport.wrapper -ne "native_add" -or
        $addReport.return_type -ne "int" -or
        @($addReport.params).Count -ne 2 -or
        $addReport.params[0].name -ne "left" -or
        $addReport.params[1].type -ne "int" -or
        -not $aliasAddReport -or
        $aliasAddReport.return_type -ne "int" -or
        $aliasAddReport.params[0].type -ne "int" -or
        -not $aliasTextReport -or
        $aliasTextReport.return_type -ne "const char*" -or
        $aliasTextReport.params[0].type -ne "const char*" -or
        -not $aliasFloatReport -or
        $aliasFloatReport.return_type -ne "double" -or
        $aliasFloatReport.params[0].type -ne "double" -or
        -not $skipReport -or
        $skipReport.emitted -ne $false -or
        $skipReport.skip_reason -notmatch "at most 4 arguments" -or
        -not $mixedSkipReport -or
        $mixedSkipReport.emitted -ne $false -or
        $mixedSkipReport.skip_reason -notmatch "string and double/float arguments" -or
        -not $limitConstant -or
        $limitConstant.name -ne "native_NATIVE_LIMIT" -or
        $limitConstant.value -ne "16" -or
        -not $hexConstant -or
        $hexConstant.value -ne "16" -or
        -not $ratioConstant -or
        $ratioConstant.value -ne "2.5" -or
        -not $negativeConstant -or
        $negativeConstant.value -ne "-2" -or
        -not $nameConstant -or
        $nameConstant.kind -ne "string" -or
        $nameConstant.value -ne "sura-native" -or
        $nameConstant.literal -ne '"sura-native"' -or
        -not $pathConstant -or
        $pathConstant.kind -ne "string" -or
        $pathConstant.value -ne "lib\native" -or
        $pathConstant.literal -ne '"lib\\native"' -or
        -not $busyEnum -or
        $busyEnum.value -ne "1" -or
        -not $errorEnum -or
        $errorEnum.name -ne "native_NATIVE_STATUS_ERROR" -or
        $errorEnum.value -ne "16" -or
        -not $lastEnum -or
        $lastEnum.value -ne "17" -or
        -not $safeEnum -or
        $safeEnum.value -ne "0" -or
        $macroConstant) {
        $bindReport | ConvertTo-Json -Depth 10 | Write-Output
        throw "expected bind-c JSON report to include emitted and skipped function details"
    }

    $generated = [System.IO.File]::ReadAllText($out, [System.Text.Encoding]::UTF8)
    if ($generated -notmatch 'lib is ffi_load\(".*native\.dll"\)') {
        throw "expected generated library load"
    }
    if ($generated -notmatch 'native_NATIVE_LIMIT is 16' -or
        $generated -notmatch 'native_NATIVE_HEX_FLAG is 16' -or
        $generated -notmatch 'native_NATIVE_RATIO is 2\.5' -or
        $generated -notmatch 'native_NATIVE_NEGATIVE is -2' -or
        $generated -notmatch 'native_NATIVE_NAME is "sura-native"' -or
        $generated -notmatch 'native_NATIVE_PATH is "lib\\\\native"' -or
        $generated -notmatch 'native_NATIVE_STATUS_BUSY is 1' -or
        $generated -notmatch 'native_NATIVE_STATUS_ERROR is 16' -or
        $generated -notmatch 'native_NATIVE_STATUS_LAST is 17' -or
        $generated -notmatch 'native_NATIVE_MODE_SAFE is 0' -or
        $generated -match 'NATIVE_MACRO') {
        throw "expected numeric #define and enum constants to be emitted and function-like macros skipped"
    }
    if ($generated -notmatch 'func native_add\(left, right\)' -or
        $generated -notmatch 'ffi_call\(lib, "add", "int\(int,int\)", left, right\)') {
        throw "expected add binding"
    }
    if ($generated -notmatch 'func native_scale\(value, factor\)' -or
        $generated -notmatch 'ffi_call\(lib, "scale", "double\(double,double\)", value, factor\)') {
        throw "expected scale binding"
    }
    if ($generated -notmatch 'func native_version\(\)' -or
        $generated -notmatch 'ffi_call\(lib, "version", "const char\*\(\)"\)') {
        throw "expected char pointer return binding"
    }
    if ($generated -notmatch 'func native_reset\(\)' -or
        $generated -notmatch 'ffi_call\(lib, "reset", "void\(\)"\)') {
        throw "expected void return binding"
    }
    if ($generated -notmatch 'func native_echo_text\(text\)' -or
        $generated -notmatch 'ffi_call\(lib, "echo_text", "const char\*\(const char\*\)", text\)') {
        throw "expected string return/string argument binding"
    }
    if ($generated -notmatch 'func native_string_plus\(text, extra\)' -or
        $generated -notmatch 'ffi_call\(lib, "string_plus", "int\(const char\*,int\)", text, extra\)') {
        throw "expected mixed string/int argument binding"
    }
    if ($generated -notmatch 'func native_string_size\(text\)' -or
        $generated -notmatch 'ffi_call\(lib, "string_size", "int\(const char\*\)", text\)') {
        throw "expected std::size_t return binding"
    }
    if ($generated -notmatch 'func native_needs_string\(text\)' -or
        $generated -notmatch 'ffi_call\(lib, "needs_string", "int\(const char\*\)", text\)') {
        throw "expected char const pointer argument binding"
    }
    if ($generated -notmatch 'func native_add_alias\(left, right\)' -or
        $generated -notmatch 'ffi_call\(lib, "add_alias", "int\(int,int\)", left, right\)' -or
        $generated -notmatch 'func native_string_size_alias\(text\)' -or
        $generated -notmatch 'ffi_call\(lib, "string_size_alias", "int\(const char\*\)", text\)' -or
        $generated -notmatch 'func native_echo_alias\(text\)' -or
        $generated -notmatch 'ffi_call\(lib, "echo_alias", "const char\*\(const char\*\)", text\)' -or
        $generated -notmatch 'func native_scale_alias\(value, factor\)' -or
        $generated -notmatch 'ffi_call\(lib, "scale_alias", "double\(double,double\)", value, factor\)') {
        throw "expected typedef/using alias bindings"
    }
    if ($generated -notmatch 'skipped too_many: ffi_call supports at most 4 arguments') {
        throw "expected too_many skip note"
    }
    if ($generated -notmatch 'skipped string_weight: string and double/float arguments cannot be mixed') {
        throw "expected mixed string/double skip note"
    }
    if ($generated -match 'should_not_bind') {
        throw "expected C++ class method to be skipped"
    }

    $bindgen = Join-Path $PSScriptRoot "bindgen_c.ps1"
    $toolOut = Join-Path $temp "native.tool.ffi.sura"
    $toolReportPath = Join-Path $temp "bind-tool-report.json"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $bindgen -Header $header -Out $toolOut -Lib $suraLib -Prefix native_ -Json $toolReportPath
    if ($LASTEXITCODE -ne 0) {
        throw "tools/bindgen_c.ps1 failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $toolOut) -or -not (Test-Path -LiteralPath $toolReportPath)) {
        throw "expected standalone bindgen output and JSON report"
    }
    $toolReport = Get-Content -Raw -Path $toolReportPath | ConvertFrom-Json
    $toolGenerated = [System.IO.File]::ReadAllText($toolOut, [System.Text.Encoding]::UTF8)
    $toolAdd = @($toolReport.functions) | Where-Object { $_.symbol -eq "add" } | Select-Object -First 1
    $toolMixed = @($toolReport.functions) | Where-Object { $_.symbol -eq "string_weight" } | Select-Object -First 1
    if ($toolReport.schema -ne "sura.bind_c.v1" -or
        $toolReport.emitted -ne 12 -or
        $toolReport.skipped -ne 2 -or
        $toolReport.emitted_constants -ne 12 -or
        -not $toolAdd -or
        $toolAdd.wrapper -ne "native_add" -or
        -not $toolMixed -or
        $toolMixed.skip_reason -notmatch "string and double/float arguments" -or
        $toolGenerated -notmatch 'native_NATIVE_LIMIT is 16' -or
        $toolGenerated -notmatch 'native_NATIVE_HEX_FLAG is 16' -or
        $toolGenerated -notmatch 'native_NATIVE_RATIO is 2\.5' -or
        $toolGenerated -notmatch 'native_NATIVE_NEGATIVE is -2' -or
        $toolGenerated -notmatch 'native_NATIVE_NAME is "sura-native"' -or
        $toolGenerated -notmatch 'native_NATIVE_PATH is "lib\\\\native"' -or
        $toolGenerated -notmatch 'native_NATIVE_STATUS_BUSY is 1' -or
        $toolGenerated -notmatch 'native_NATIVE_STATUS_ERROR is 16' -or
        $toolGenerated -notmatch 'native_NATIVE_STATUS_LAST is 17' -or
        $toolGenerated -notmatch 'native_NATIVE_MODE_SAFE is 0' -or
        $toolGenerated -notmatch 'func native_add\(left, right\)' -or
        $toolGenerated -notmatch 'func native_add_alias\(left, right\)' -or
        $toolGenerated -notmatch 'func native_echo_alias\(text\)' -or
        $toolGenerated -notmatch 'func native_echo_text\(text\)' -or
        $toolGenerated -notmatch 'skipped string_weight') {
        $toolReport | ConvertTo-Json -Depth 10 | Write-Output
        throw "expected standalone bindgen to match surapkg bind-c coverage"
    }
    & $Engine --check $toolOut
    if ($LASTEXITCODE -ne 0) {
        throw "standalone bindgen output did not pass Sura --check"
    }

    & $Engine --check $out
    if ($LASTEXITCODE -ne 0) {
        throw "generated binding did not pass Sura --check"
    }

    [System.IO.File]::AppendAllText(
        $out,
        @"

assert_eq(native_add(2, 3), 5)
assert_eq(native_scale(2.0, 4.0), 8.0)
assert_eq(native_version(), "1.2.3")
assert_eq(native_NATIVE_LIMIT, 16)
assert_eq(native_NATIVE_HEX_FLAG, 16)
assert_eq(native_NATIVE_RATIO, 2.5)
assert_eq(native_NATIVE_NEGATIVE, -2)
assert_eq(native_NATIVE_NAME, "sura-native")
assert_eq(native_NATIVE_PATH, "lib\\native")
assert_eq(native_NATIVE_STATUS_OK, 0)
assert_eq(native_NATIVE_STATUS_BUSY, 1)
assert_eq(native_NATIVE_STATUS_ERROR, 16)
assert_eq(native_NATIVE_STATUS_LAST, 17)
assert_eq(native_NATIVE_MODE_FAST, -1)
assert_eq(native_NATIVE_MODE_SAFE, 0)
assert_eq(native_needs_string("hello"), 5)
assert_eq(native_string_size("hello"), 5)
assert_eq(native_string_plus("abcd", 5), 9)
assert_eq(native_echo_text("sura-text"), "sura-text")
assert_eq(native_add_alias(2, 3), 15)
assert_eq(native_string_size_alias("hello"), 5)
assert_eq(native_echo_alias("alias-text"), "alias-text")
assert_eq(native_scale_alias(2.0, 4.0), 9.0)
"@,
        $utf8NoBom
    )
    & $Engine $out
    if ($LASTEXITCODE -ne 0) {
        throw "generated binding did not run through string FFI"
    }

    $namespaceScript = Join-Path $temp "ffi_namespace.sura"
    [System.IO.File]::WriteAllText(
        $namespaceScript,
        @"
use ffi

lib is ffi.load("$suraLib")
assert_eq(ffi.call(lib, "add", "int(int,int)", 2, 3), 5)
assert_eq(ffi.call(lib, "scale", "double(double,double)", 2.0, 4.0), 8.0)
assert_eq(ffi.call(lib, "version", "const char*()"), "1.2.3")
assert_eq(ffi.call(lib, "needs_string", "int(const char*)", "hello"), 5)
print "ffi_namespace_smoke: PASS"
"@,
        $utf8NoBom
    )
    & $Engine $namespaceScript
    if ($LASTEXITCODE -ne 0) {
        throw "namespaced ffi module did not run through native calls"
    }

    Write-Output "bind_c_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
