param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_engine_lint_" + [System.Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $good = Join-Path $root "good.sura"
    Set-Content -LiteralPath $good -Encoding UTF8 @"
if true then
  print("ok")
end
"@

    $passOutput = & $enginePath --lint $root 2>&1
    $passCode = $LASTEXITCODE
    if ($passCode -ne 0 -or ($passOutput -join "`n") -notmatch "Sura lint: 1 passed, 0 failed") {
        $passOutput | Write-Host
        throw "expected passing --lint run"
    }

    $risky = Join-Path $root "risky.sura"
    Set-Content -LiteralPath $risky -Encoding UTF8 @"
use tool
spec is tool.spec("http_get", {url: "https://example.com"})
print(tool.call(spec))
wide_policy is {}
print(tool.call_policy(spec, wide_policy))
prefixless_policy is {tools: ["http_get"]}
print(tool.call_policy(spec, prefixless_policy))
print(http_get("https://example.com"))
"@

    $headers = Join-Path $root "headers.sura"
    Set-Content -LiteralPath $headers -Encoding UTF8 @"
headers is http.headers_merge(http.auth_bearer("secret-token"), {"X-Api-Key": "key"})
print(headers)
safe_headers is headers_redact(headers)
print(safe_headers)
"@

    $warnOutput = & $enginePath --lint $root 2>&1
    $warnCode = $LASTEXITCODE
    $warnText = $warnOutput -join "`n"
    if ($warnCode -ne 0 -or
        $warnText -notmatch "risky network access" -or
        $warnText -notmatch "risky unpolicyed tool call" -or
        $warnText -notmatch "risky weak tool policy" -or
        $warnText -notmatch "risky unredacted sensitive headers" -or
        $warnText -notmatch "warning") {
        $warnOutput | Write-Host
        throw "expected risky API warning without failure"
    }

    $legacy = Join-Path $root "legacy.sura"
    Set-Content -LiteralPath $legacy -Encoding UTF8 @"
print "legacy"
"@

    $legacyOutput = & $enginePath --lint $legacy 2>&1
    $legacyCode = $LASTEXITCODE
    $legacyText = $legacyOutput -join "`n"
    if ($legacyCode -ne 0 -or
        $legacyText -notmatch "legacy command syntax" -or
        $legacyText -notmatch "1 warning") {
        $legacyOutput | Write-Host
        throw "expected legacy command syntax lint warning without failure"
    }

    $bad = Join-Path $root "bad.sura"
    Set-Content -LiteralPath $bad -Encoding UTF8 @"
end
"@

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $failOutput = & $enginePath --lint $root 2>&1
    $failCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    $failText = $failOutput -join "`n"
    if ($failCode -eq 0 -or $failText -notmatch "unmatched end" -or $failText -notmatch "bad\.sura") {
        $failOutput | Write-Host
        throw "expected structural lint failure"
    }

    Write-Host "[PASS] engine lint smoke"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
