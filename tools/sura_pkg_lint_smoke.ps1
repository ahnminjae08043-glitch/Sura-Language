param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_lint_" + [System.Guid]::NewGuid().ToString("N"))
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

    $good = Join-Path $temp "good_pkg"
    Write-Text (Join-Path $good "sura.pkg.json") @"
{
  "name": "good_pkg",
  "version": "1.0.0",
  "main": "src/good_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $good "src/good_pkg.sura") @"
func add do
  return 2 + 3
end
"@
    $goodJson = Join-Path $temp "good-lint.json"
    $goodResult = Run-Pkg -PkgArgs @("lint", $good, "--json", $goodJson)
    if ($goodResult.Code -ne 0 -or
        $goodResult.Output -notmatch 'Sura lint: 1 checked, 0 error\(s\), 0 warning\(s\)' -or
        $goodResult.Output -notmatch "lint passed" -or
        -not (Test-Path -LiteralPath $goodJson)) {
        Write-Output $goodResult.Output
        throw "expected clean package lint to pass and write JSON"
    }
    $goodReport = [System.IO.File]::ReadAllText($goodJson, [System.Text.Encoding]::UTF8)
    if ($goodReport -notmatch '"passed"\s*:\s*true' -or
        $goodReport -notmatch '"files_checked"\s*:\s*1' -or
        $goodReport -notmatch '"warning_count"\s*:\s*0' -or
        $goodReport -notmatch '"error_count"\s*:\s*0') {
        throw "expected clean lint JSON counts"
    }

    $warn = Join-Path $temp "warn_pkg"
    Write-Text (Join-Path $warn "sura.pkg.json") @"
{
  "name": "warn_pkg",
  "version": "1.0.0",
  "main": "src/warn_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $warn "src/warn_pkg.sura") @"
print(http_get("https://example.com"))
cmd_run_checked("echo lint")
use tool
spec is tool.spec("http_get", {url: "https://example.com"})
wide_policy is {}
print(tool.call_policy(spec, wide_policy))
prefixless_policy is {tools: ["http_get"]}
print(tool.call_policy(spec, prefixless_policy))
headers is http.headers_merge(http.auth_bearer("secret-token"), {"X-Api-Key": "key"})
print(headers)
safe_headers is headers_redact(headers)
print(safe_headers)
"@
    $warnJson = Join-Path $temp "warn-lint.json"
    $warnResult = Run-Pkg -PkgArgs @("lint", $warn, "--json", $warnJson)
    if ($warnResult.Code -ne 0 -or
        $warnResult.Output -notmatch "risky network access" -or
        $warnResult.Output -notmatch "risky shell execution" -or
        $warnResult.Output -notmatch "risky weak tool policy" -or
        $warnResult.Output -notmatch "risky unredacted sensitive headers" -or
        $warnResult.Output -notmatch '0 error\(s\), 5 warning\(s\)') {
        Write-Output $warnResult.Output
        throw "expected risky API lint warning without failure"
    }
    $warnReport = [System.IO.File]::ReadAllText($warnJson, [System.Text.Encoding]::UTF8)
    if ($warnReport -notmatch '"warning_count"\s*:\s*5' -or
        $warnReport -notmatch '"kind"\s*:\s*"unredacted_sensitive_headers"' -or
        $warnReport -notmatch '"kind"\s*:\s*"weak_tool_policy"') {
        throw "expected unredacted headers and weak policy lint JSON findings"
    }
    $warnFail = Run-Pkg -PkgArgs @("lint", $warn, "--fail-on-warning")
    if ($warnFail.Code -eq 0 -or $warnFail.Output -notmatch "lint found 5 warning") {
        Write-Output $warnFail.Output
        throw "expected --fail-on-warning to fail"
    }

    $legacy = Join-Path $temp "legacy_pkg"
    Write-Text (Join-Path $legacy "sura.pkg.json") @"
{
  "name": "legacy_pkg",
  "version": "1.0.0",
  "main": "src/legacy_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $legacy "src/legacy_pkg.sura") @"
print "legacy"
"@
    $legacyJson = Join-Path $temp "legacy-lint.json"
    $legacyResult = Run-Pkg -PkgArgs @("lint", $legacy, "--json", $legacyJson)
    if ($legacyResult.Code -ne 0 -or
        $legacyResult.Output -notmatch "legacy command syntax" -or
        $legacyResult.Output -notmatch '0 error\(s\), 1 warning\(s\)' -or
        -not (Test-Path -LiteralPath $legacyJson)) {
        Write-Output $legacyResult.Output
        throw "expected legacy command syntax warning without failure"
    }
    $legacyReport = [System.IO.File]::ReadAllText($legacyJson, [System.Text.Encoding]::UTF8)
    if ($legacyReport -notmatch '"warning_count"\s*:\s*1' -or
        $legacyReport -notmatch '"kind"\s*:\s*"legacy_command_syntax"') {
        throw "expected legacy command syntax lint JSON finding"
    }

    $bad = Join-Path $temp "bad_pkg"
    Write-Text (Join-Path $bad "sura.pkg.json") @"
{
  "name": "bad_pkg",
  "version": "1.0.0",
  "main": "src/bad_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $bad "src/bad_pkg.sura") @"
if true then
  print("missing end")
"@
    $badJson = Join-Path $temp "bad-lint.json"
    $badResult = Run-Pkg -PkgArgs @("lint", $bad, "--json", $badJson)
    if ($badResult.Code -eq 0 -or
        $badResult.Output -notmatch "unclosed block" -or
        $badResult.Output -notmatch '1 error\(s\)' -or
        -not (Test-Path -LiteralPath $badJson)) {
        Write-Output $badResult.Output
        throw "expected structural lint error and JSON report"
    }
    $badReport = [System.IO.File]::ReadAllText($badJson, [System.Text.Encoding]::UTF8)
    if ($badReport -notmatch '"passed"\s*:\s*false' -or
        $badReport -notmatch '"severity"\s*:\s*"error"' -or
        $badReport -notmatch '"kind"\s*:\s*"unclosed_block"') {
        throw "expected failing lint JSON finding"
    }

    "pkg_lint_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
