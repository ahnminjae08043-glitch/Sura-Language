param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$surapkgPath = Resolve-Path (Join-Path $repo $Surapkg)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Run-Audit {
    param([string]$Path)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $surapkgPath audit $Path 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_plugin_manifest_audit_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temp | Out-Null

try {
    [System.IO.File]::WriteAllText(
        (Join-Path $temp "sura.pkg.json"),
        "{`n  `"name`": `"plugin_manifest_audit`",`n  `"version`": `"0.1.0`",`n  `"dependencies`": {}`n}`n",
        $utf8NoBom
    )
    $nativeDir = Join-Path $temp "native"
    $srcDir = Join-Path $temp "src"
    New-Item -ItemType Directory -Path $nativeDir | Out-Null
    New-Item -ItemType Directory -Path $srcDir | Out-Null
    $libPath = Join-Path $nativeDir "sample_plugin.dll"
    [System.IO.File]::WriteAllText($libPath, "fake native plugin bytes", $utf8NoBom)
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $libPath).Hash.ToLowerInvariant()
    $manifestPath = Join-Path $nativeDir "sample.sura-plugin.json"
    $policyPath = Join-Path $temp "sura.plugins.json"
    $sourcePath = Join-Path $srcDir "plugin_manifest_audit.sura"

    $validManifest = @"
{
  "path": "sample_plugin.dll",
  "name": "sample_plugin",
  "version": "0.1.0",
  "sha256": "$hash",
  "exports": ["native_add", "native_mul"],
  "host_capabilities": ["memory"],
  "max_memory_bytes": 64,
  "max_call_ms": 100
}
"@
    [System.IO.File]::WriteAllText($manifestPath, $validManifest, $utf8NoBom)
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )
    [System.IO.File]::WriteAllText(
        $sourcePath,
        "use plugin`r`nplug is plugin.load_manifest(`"native/sample.sura-plugin.json`")`r`nprint plugin.call(plug, `"native_add`", 1, 2)`r`nplugin.unload(plug)`r`n",
        $utf8NoBom
    )
    $valid = Run-Audit $temp
    if ($valid.Code -ne 0) {
        Write-Output $valid.Output
        throw "valid plugin manifest audit failed"
    }
    $cancelManifest = $validManifest.Replace('"host_capabilities": ["memory"]', '"host_capabilities": ["memory", "cancel"]')
    [System.IO.File]::WriteAllText($manifestPath, $cancelManifest, $utf8NoBom)
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`", `"cancel`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )
    $validCancel = Run-Audit $temp
    if ($validCancel.Code -ne 0) {
        Write-Output $validCancel.Output
        throw "cancel host capability plugin manifest audit failed"
    }
    [System.IO.File]::WriteAllText($manifestPath, $validManifest, $utf8NoBom)
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )

    [System.IO.File]::WriteAllText(
        $sourcePath,
        "use plugin`r`nplug is plugin.load_manifest(`"native/sample.sura-plugin.json`")`r`nprint plugin.call(plug, `"native_mul`", 2, 3)`r`nplugin.unload(plug)`r`n",
        $utf8NoBom
    )
    $policyDeniedExport = Run-Audit $temp
    if ($policyDeniedExport.Code -eq 0 -or $policyDeniedExport.Output -notmatch "plugin_call export not allowed") {
        Write-Output $policyDeniedExport.Output
        throw "plugin_call export outside package allowed_exports was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $sourcePath,
        "use plugin`r`nplug is plugin.load_manifest(`"native/sample.sura-plugin.json`")`r`nprint plugin.call(plug, `"native_add`", 1, 2)`r`nplugin.unload(plug)`r`n",
        $utf8NoBom
    )
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"network`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )
    $badPolicyHostCapability = Run-Audit $temp
    if ($badPolicyHostCapability.Code -eq 0 -or $badPolicyHostCapability.Output -notmatch "unsupported host capability") {
        Write-Output $badPolicyHostCapability.Output
        throw "unsupported package host capability was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"log`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )
    $policyUndeclaredCapability = Run-Audit $temp
    if ($policyUndeclaredCapability.Code -eq 0 -or $policyUndeclaredCapability.Output -notmatch "host capability not declared") {
        Write-Output $policyUndeclaredCapability.Output
        throw "package host capability outside plugin manifest was not rejected"
    }

    Remove-Item -LiteralPath $policyPath -Force
    $missingPolicy = Run-Audit $temp
    if ($missingPolicy.Code -eq 0 -or $missingPolicy.Output -notmatch "plugin policy manifest missing") {
        Write-Output $missingPolicy.Output
        throw "plugin manifest without package policy was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )

    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": -1,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )
    $badPolicyMaxMemory = Run-Audit $temp
    if ($badPolicyMaxMemory.Code -eq 0 -or $badPolicyMaxMemory.Output -notmatch "max_memory_bytes") {
        Write-Output $badPolicyMaxMemory.Output
        throw "negative package max_memory_bytes was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )

    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": -1`n}`n",
        $utf8NoBom
    )
    $badPolicyMaxCallMs = Run-Audit $temp
    if ($badPolicyMaxCallMs.Code -eq 0 -or $badPolicyMaxCallMs.Output -notmatch "max_call_ms") {
        Write-Output $badPolicyMaxCallMs.Output
        throw "negative package max_call_ms was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $policyPath,
        "{`n  `"version`": 1,`n  `"sandbox`": `"manifest-locked`",`n  `"manifests`": [`"native/sample.sura-plugin.json`"],`n  `"allowed_exports`": [`"native_add`"],`n  `"host_capabilities`": [`"memory`"],`n  `"max_memory_bytes`": 64,`n  `"max_call_ms`": 100`n}`n",
        $utf8NoBom
    )

    [System.IO.File]::WriteAllText(
        $sourcePath,
        "use plugin`r`nplug is plugin.load(`"native/sample_plugin.dll`")`r`n",
        $utf8NoBom
    )
    $rawPluginLoad = Run-Audit $temp
    if ($rawPluginLoad.Code -eq 0 -or $rawPluginLoad.Output -notmatch "direct plugin_load requires") {
        Write-Output $rawPluginLoad.Output
        throw "direct plugin_load was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $sourcePath,
        "use plugin`r`nplug is plugin.load_manifest(`"native/unlisted.sura-plugin.json`")`r`n",
        $utf8NoBom
    )
    $unlistedLoad = Run-Audit $temp
    if ($unlistedLoad.Code -eq 0 -or $unlistedLoad.Output -notmatch "not listed") {
        Write-Output $unlistedLoad.Output
        throw "unlisted plugin_load_manifest path was not rejected"
    }
    [System.IO.File]::WriteAllText(
        $sourcePath,
        "use plugin`r`nplug is plugin.load_manifest(`"native/sample.sura-plugin.json`")`r`nprint plugin.call(plug, `"native_add`", 1, 2)`r`nplugin.unload(plug)`r`n",
        $utf8NoBom
    )

    $badHashManifest = $validManifest.Replace($hash, ("0" * 64))
    [System.IO.File]::WriteAllText($manifestPath, $badHashManifest, $utf8NoBom)
    $badHash = Run-Audit $temp
    if ($badHash.Code -eq 0 -or $badHash.Output -notmatch "sha256 mismatch") {
        Write-Output $badHash.Output
        throw "bad hash plugin manifest was not rejected"
    }

    $badPathManifest = $validManifest.Replace('"path": "sample_plugin.dll"', '"path": "../sample_plugin.dll"')
    [System.IO.File]::WriteAllText($manifestPath, $badPathManifest, $utf8NoBom)
    $badPath = Run-Audit $temp
    if ($badPath.Code -eq 0 -or $badPath.Output -notmatch "path must not escape") {
        Write-Output $badPath.Output
        throw "escaping plugin manifest path was not rejected"
    }

    $badExportsManifest = $validManifest.Replace('"exports": ["native_add", "native_mul"]', '"exports": []')
    [System.IO.File]::WriteAllText($manifestPath, $badExportsManifest, $utf8NoBom)
    $badExports = Run-Audit $temp
    if ($badExports.Code -eq 0 -or $badExports.Output -notmatch "exports") {
        Write-Output $badExports.Output
        throw "empty plugin manifest export allow-list was not rejected"
    }

    $badHostCapabilityManifest = $validManifest.Replace('"host_capabilities": ["memory"]', '"host_capabilities": ["network"]')
    [System.IO.File]::WriteAllText($manifestPath, $badHostCapabilityManifest, $utf8NoBom)
    $badHostCapability = Run-Audit $temp
    if ($badHostCapability.Code -eq 0 -or $badHostCapability.Output -notmatch "unsupported host capability") {
        Write-Output $badHostCapability.Output
        throw "unsupported plugin host capability was not rejected"
    }

    $badMaxMemoryManifest = $validManifest.Replace('"max_memory_bytes": 64', '"max_memory_bytes": -1')
    [System.IO.File]::WriteAllText($manifestPath, $badMaxMemoryManifest, $utf8NoBom)
    $badMaxMemory = Run-Audit $temp
    if ($badMaxMemory.Code -eq 0 -or $badMaxMemory.Output -notmatch "max_memory_bytes") {
        Write-Output $badMaxMemory.Output
        throw "negative plugin max_memory_bytes was not rejected"
    }

    $badMaxCallMsManifest = $validManifest.Replace('"max_call_ms": 100', '"max_call_ms": -1')
    [System.IO.File]::WriteAllText($manifestPath, $badMaxCallMsManifest, $utf8NoBom)
    $badMaxCallMs = Run-Audit $temp
    if ($badMaxCallMs.Code -eq 0 -or $badMaxCallMs.Output -notmatch "max_call_ms") {
        Write-Output $badMaxCallMs.Output
        throw "negative plugin max_call_ms was not rejected"
    }

    "plugin_manifest_audit_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
