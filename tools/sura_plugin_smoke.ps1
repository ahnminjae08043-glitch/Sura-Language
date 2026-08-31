param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$enginePath = Resolve-Path (Join-Path $repo $Engine)

$compiler = $null
foreach ($candidate in @("gcc", "cc", "g++")) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($cmd) {
        $compiler = $cmd.Source
        break
    }
}
if (-not $compiler) {
    throw "No C compiler found on PATH. Install gcc/cc/g++ to run the plugin smoke test."
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_plugin_smoke_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temp | Out-Null

try {
    $sourcePath = Join-Path $temp "sura_plugin_sample.c"
    (Get-Content (Join-Path $repo "examples\sura_plugin_sample.c") -Raw).
        Replace('#include "../sura_plugin.h"', '#include "sura_plugin.h"') |
        Set-Content -Path $sourcePath -Encoding UTF8
    Copy-Item (Join-Path $repo "sura_plugin.h") (Join-Path $temp "sura_plugin.h")

    $onWindows = $env:OS -eq "Windows_NT"
    $libName = if ($onWindows) { "sura_sample_plugin.dll" } elseif ($IsMacOS) { "libsura_sample_plugin.dylib" } else { "libsura_sample_plugin.so" }
    $pluginPath = Join-Path $temp $libName

    $compileArgs = @("-shared", "-O2", "-I", $temp, "-o", $pluginPath, $sourcePath)
    if ($IsMacOS) {
        $compileArgs = @("-dynamiclib", "-fPIC", "-O2", "-I", $temp, "-o", $pluginPath, $sourcePath)
    } elseif (-not $onWindows) {
        $compileArgs = @("-shared", "-fPIC", "-O2", "-I", $temp, "-o", $pluginPath, $sourcePath)
    }
    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin compile failed with exit code $LASTEXITCODE"
    }

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pluginPath).Hash.ToLowerInvariant()
    $manifestPath = Join-Path $temp "sura_sample_plugin.sura-plugin.json"
    $manifest = @"
{
  "path": "$libName",
  "name": "sura_sample_plugin",
  "version": "0.1.0",
  "sha256": "$hash",
  "exports": ["native_add", "native_call_count", "native_lifecycle_count", "native_spin_ms"],
  "host_capabilities": ["memory", "cancel"],
  "max_memory_bytes": 64,
  "max_call_ms": 100
}
"@
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($manifestPath, $manifest, $utf8NoBom)

    $suraPluginPath = ($pluginPath -replace "\\", "/").Replace('"', '\"')
    $suraManifestPath = ($manifestPath -replace "\\", "/").Replace('"', '\"')
    $scriptPath = Join-Path $temp "plugin_smoke.sura"
    $script = @"
use plugin
direct is plugin.load("$suraPluginPath")
direct_info is plugin.info(direct)
assert(direct_info.has_state, "direct plugin should expose state")
assert(direct_info.has_on_load, "direct plugin should expose on_load")
assert(direct_info.has_on_unload, "direct plugin should expose on_unload")
assert_eq(plugin.call(direct, "native_lifecycle_count"), 1)
assert_eq(plugin.call(direct, "native_call_count"), 0)
assert_eq(plugin.call(direct, "native_mul", 2, 4), 8)
assert_eq(plugin.call(direct, "native_call_count"), 1)
assert_eq(plugin.call(direct, "native_add", 1, 1), 2)
assert_eq(plugin.call(direct, "native_call_count"), 2)
assert(plugin.unload(direct), "direct plugin should unload")

plug is plugin.load_manifest("$suraManifestPath")
info is plugin.info(plug)
assert_eq(info.name, "sura_sample_plugin")
assert_eq(info.version, "0.1.0")
assert_eq(info.policy, "manifest")
assert_eq(info.max_memory_bytes, 64)
assert_eq(info.max_call_ms, 100)
assert_contains(info.host_capabilities, "cancel")
assert(info.has_on_load, "manifest plugin should expose on_load")
assert(info.has_on_unload, "manifest plugin should expose on_unload")
assert(info.memory_bytes > 0, "manifest plugin should track host memory")
assert(info.has_state, "manifest plugin should expose state")
assert_eq(plugin.call(plug, "native_lifecycle_count"), 1)
assert_eq(plugin.call(plug, "native_call_count"), 0)
assert_eq(plugin.call(plug, "native_add", 2, 3), 5)
assert_eq(plugin.call(plug, "native_add", -4, 9), 5)
assert_eq(plugin.call(plug, "native_call_count"), 2)
after_calls is plugin.info(plug)
assert(after_calls.call_count >= 5, "plugin.info should track native call count")
assert(after_calls.total_call_ms >= after_calls.last_call_ms, "plugin.info should track native call duration")
assert(plugin.unload(plug), "plugin should unload")
print "plugin_smoke: PASS"
"@
    [System.IO.File]::WriteAllText($scriptPath, $script, $utf8NoBom)

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $enginePath $scriptPath 2>&1 | ForEach-Object { "$_" }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    if ($exitCode -ne 0) {
        $output | ForEach-Object { Write-Output $_ }
        throw "Sura plugin smoke failed with exit code $exitCode"
    }

    $deniedPath = Join-Path $temp "plugin_denied.sura"
    $deniedScript = @"
use plugin
plug is plugin.load_manifest("$suraManifestPath")
plugin.call(plug, "native_mul", 2, 3)
"@
    [System.IO.File]::WriteAllText($deniedPath, $deniedScript, $utf8NoBom)
    $ErrorActionPreference = "Continue"
    $deniedOutput = & $enginePath $deniedPath 2>&1 | ForEach-Object { "$_" }
    $deniedExit = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    if ($deniedExit -eq 0 -or (($deniedOutput -join "`n") -notmatch "export not allowed by manifest")) {
        $deniedOutput | ForEach-Object { Write-Output $_ }
        throw "Plugin manifest policy smoke failed: native_mul was not blocked"
    }

    $runtimePolicyPath = Join-Path $temp "sura.plugins.json"
    $runtimePolicy = @"
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["sura_sample_plugin.sura-plugin.json"],
  "allowed_exports": ["native_add"],
  "host_capabilities": ["memory"],
  "max_memory_bytes": 64,
  "max_call_ms": 100
}
"@
    [System.IO.File]::WriteAllText($runtimePolicyPath, $runtimePolicy, $utf8NoBom)

    $oldPluginPolicy = $env:SURA_PLUGIN_POLICY
    $env:SURA_PLUGIN_POLICY = "manifest-locked"
    Push-Location $temp
    try {
        $lockedDirectPath = Join-Path $temp "plugin_locked_direct.sura"
        $lockedDirectScript = @"
use plugin
plugin.load("$suraPluginPath")
"@
        [System.IO.File]::WriteAllText($lockedDirectPath, $lockedDirectScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedDirectOutput = & $enginePath $lockedDirectPath 2>&1 | ForEach-Object { "$_" }
        $lockedDirectExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedDirectExit -eq 0 -or (($lockedDirectOutput -join "`n") -notmatch "direct plugin loading blocked")) {
            $lockedDirectOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: direct plugin load was not blocked"
        }

        $lockedManifestPath = Join-Path $temp "plugin_locked_manifest.sura"
        $lockedManifestScript = @"
use plugin
plug is plugin.load_manifest("$suraManifestPath")
info is plugin.info(plug)
assert_contains(info.allowed_exports, "native_add")
assert_contains(info.host_capabilities, "memory")
assert_eq(info.max_memory_bytes, 64)
assert_eq(info.max_call_ms, 100)
assert(info.memory_bytes > 0, "runtime policy should track host memory")
assert_eq(plugin.call(plug, "native_add", 4, 6), 10)
assert(plugin.unload(plug), "plugin should unload under runtime policy")
print "plugin_runtime_policy: PASS"
"@
        [System.IO.File]::WriteAllText($lockedManifestPath, $lockedManifestScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedManifestOutput = & $enginePath $lockedManifestPath 2>&1 | ForEach-Object { "$_" }
        $lockedManifestExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedManifestExit -ne 0 -or (($lockedManifestOutput -join "`n") -notmatch "plugin_runtime_policy: PASS")) {
            $lockedManifestOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: listed manifest was not allowed"
        }

        $lockedExportDeniedPath = Join-Path $temp "plugin_locked_export_denied.sura"
        $lockedExportDeniedScript = @"
use plugin
plug is plugin.load_manifest("$suraManifestPath")
plugin.call(plug, "native_call_count")
"@
        [System.IO.File]::WriteAllText($lockedExportDeniedPath, $lockedExportDeniedScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedExportDeniedOutput = & $enginePath $lockedExportDeniedPath 2>&1 | ForEach-Object { "$_" }
        $lockedExportDeniedExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedExportDeniedExit -eq 0 -or (($lockedExportDeniedOutput -join "`n") -notmatch "export not allowed by manifest")) {
            $lockedExportDeniedOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: package allowed_exports did not narrow plugin exports"
        }

        $runtimePolicyNoMemory = @"
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["sura_sample_plugin.sura-plugin.json"],
  "allowed_exports": ["native_add"],
  "host_capabilities": []
}
"@
        [System.IO.File]::WriteAllText($runtimePolicyPath, $runtimePolicyNoMemory, $utf8NoBom)
        $lockedNoMemoryPath = Join-Path $temp "plugin_locked_no_memory.sura"
        $lockedNoMemoryScript = @"
use plugin
plugin.load_manifest("$suraManifestPath")
"@
        [System.IO.File]::WriteAllText($lockedNoMemoryPath, $lockedNoMemoryScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedNoMemoryOutput = & $enginePath $lockedNoMemoryPath 2>&1 | ForEach-Object { "$_" }
        $lockedNoMemoryExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedNoMemoryExit -eq 0 -or (($lockedNoMemoryOutput -join "`n") -notmatch "plugin rejected by ABI/version check")) {
            $lockedNoMemoryOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: host_capabilities did not disable plugin memory allocation"
        }

        $runtimePolicyLowMemory = @"
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["sura_sample_plugin.sura-plugin.json"],
  "allowed_exports": ["native_add"],
  "host_capabilities": ["memory"],
  "max_memory_bytes": 1
}
"@
        [System.IO.File]::WriteAllText($runtimePolicyPath, $runtimePolicyLowMemory, $utf8NoBom)
        $lockedLowMemoryPath = Join-Path $temp "plugin_locked_low_memory.sura"
        $lockedLowMemoryScript = @"
use plugin
plugin.load_manifest("$suraManifestPath")
"@
        [System.IO.File]::WriteAllText($lockedLowMemoryPath, $lockedLowMemoryScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedLowMemoryOutput = & $enginePath $lockedLowMemoryPath 2>&1 | ForEach-Object { "$_" }
        $lockedLowMemoryExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedLowMemoryExit -eq 0 -or (($lockedLowMemoryOutput -join "`n") -notmatch "plugin rejected by ABI/version check")) {
            $lockedLowMemoryOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: max_memory_bytes did not limit host allocation"
        }

        $runtimePolicyLowCallMs = @"
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["sura_sample_plugin.sura-plugin.json"],
  "allowed_exports": ["native_spin_ms"],
  "host_capabilities": ["memory"],
  "max_memory_bytes": 64,
  "max_call_ms": 1
}
"@
        [System.IO.File]::WriteAllText($runtimePolicyPath, $runtimePolicyLowCallMs, $utf8NoBom)
        $lockedLowCallMsPath = Join-Path $temp "plugin_locked_low_call_ms.sura"
        $lockedLowCallMsScript = @"
use plugin
plug is plugin.load_manifest("$suraManifestPath")
plugin.call(plug, "native_spin_ms", 20)
"@
        [System.IO.File]::WriteAllText($lockedLowCallMsPath, $lockedLowCallMsScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedLowCallMsOutput = & $enginePath $lockedLowCallMsPath 2>&1 | ForEach-Object { "$_" }
        $lockedLowCallMsExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedLowCallMsExit -eq 0 -or (($lockedLowCallMsOutput -join "`n") -notmatch "exceeded max_call_ms")) {
            $lockedLowCallMsOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: max_call_ms did not limit native call duration"
        }

        $runtimePolicyCancelableCallMs = @"
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["sura_sample_plugin.sura-plugin.json"],
  "allowed_exports": ["native_spin_ms"],
  "host_capabilities": ["memory", "cancel"],
  "max_memory_bytes": 64,
  "max_call_ms": 1
}
"@
        [System.IO.File]::WriteAllText($runtimePolicyPath, $runtimePolicyCancelableCallMs, $utf8NoBom)
        $lockedCancelableCallMsPath = Join-Path $temp "plugin_locked_cancel_call_ms.sura"
        $lockedCancelableCallMsScript = @"
use plugin
plug is plugin.load_manifest("$suraManifestPath")
plugin.call(plug, "native_spin_ms", 20)
"@
        [System.IO.File]::WriteAllText($lockedCancelableCallMsPath, $lockedCancelableCallMsScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedCancelableCallMsOutput = & $enginePath $lockedCancelableCallMsPath 2>&1 | ForEach-Object { "$_" }
        $lockedCancelableCallMsExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedCancelableCallMsExit -eq 0 -or (($lockedCancelableCallMsOutput -join "`n") -notmatch "native export cancelled")) {
            $lockedCancelableCallMsOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: cancel host capability did not provide cooperative native cancellation"
        }
        [System.IO.File]::WriteAllText($runtimePolicyPath, $runtimePolicy, $utf8NoBom)

        $unlistedManifestPath = Join-Path $temp "unlisted.sura-plugin.json"
        [System.IO.File]::WriteAllText($unlistedManifestPath, $manifest, $utf8NoBom)
        $suraUnlistedManifestPath = ($unlistedManifestPath -replace "\\", "/").Replace('"', '\"')
        $lockedUnlistedPath = Join-Path $temp "plugin_locked_unlisted.sura"
        $lockedUnlistedScript = @"
use plugin
plugin.load_manifest("$suraUnlistedManifestPath")
"@
        [System.IO.File]::WriteAllText($lockedUnlistedPath, $lockedUnlistedScript, $utf8NoBom)
        $ErrorActionPreference = "Continue"
        $lockedUnlistedOutput = & $enginePath $lockedUnlistedPath 2>&1 | ForEach-Object { "$_" }
        $lockedUnlistedExit = $LASTEXITCODE
        $ErrorActionPreference = $oldErrorActionPreference
        if ($lockedUnlistedExit -eq 0 -or (($lockedUnlistedOutput -join "`n") -notmatch "manifest not allowed by sura.plugins.json")) {
            $lockedUnlistedOutput | ForEach-Object { Write-Output $_ }
            throw "Runtime plugin policy smoke failed: unlisted manifest was not blocked"
        }
    }
    finally {
        Pop-Location
        $env:SURA_PLUGIN_POLICY = $oldPluginPolicy
        $ErrorActionPreference = $oldErrorActionPreference
    }

    $output
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
