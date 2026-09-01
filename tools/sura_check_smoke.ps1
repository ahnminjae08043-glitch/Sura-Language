param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_check_" + [System.Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    Set-Content -LiteralPath (Join-Path $root "good.sura") -Encoding UTF8 @"
x is 1
assert(x == 1)
"@

    $passOutput = & $enginePath --check $root 2>&1
    $passCode = $LASTEXITCODE
    if ($passCode -ne 0 -or ($passOutput -join "`n") -notmatch "Sura check: 1 passed, 0 failed") {
        $passOutput | Write-Host
        throw "expected passing --check run"
    }

    Set-Content -LiteralPath (Join-Path $root "bad.sura") -Encoding UTF8 @"
if true then
    print "missing end"
"@

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $failOutput = & $enginePath --check $root 2>&1
    $failCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    $failText = $failOutput -join "`n"
    if ($failCode -eq 0 -or $failText -notmatch "\[FAIL\]" -or $failText -notmatch "bad\.sura") {
        $failOutput | Write-Host
        throw "expected failing --check run"
    }

    $multiBad = Join-Path $root "multi_bad.sura"
    Set-Content -LiteralPath $multiBad -Encoding UTF8 @"
first is
print (1
second is
"@

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $multiOutput = & $enginePath --check $multiBad 2>&1
    $multiCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    $multiText = $multiOutput -join "`n"
    $parseErrorCount = ([regex]::Matches($multiText, "\[Sura Parse Error\]")).Count
    if ($multiCode -eq 0 -or $parseErrorCount -lt 2) {
        $multiOutput | Write-Host
        throw "expected parser recovery to report multiple syntax errors"
    }

    $strictGood = Join-Path $root "strict_good.sura"
    Set-Content -LiteralPath $strictGood -Encoding UTF8 @"
print("strict ok")
"@
    $strictGoodOutput = & $enginePath --strict-syntax --check $strictGood 2>&1
    $strictGoodCode = $LASTEXITCODE
    if ($strictGoodCode -ne 0 -or ($strictGoodOutput -join "`n") -notmatch "Sura check: 1 passed, 0 failed") {
        $strictGoodOutput | Write-Host
        throw "expected strict syntax to accept function-call syntax"
    }

    $strictBad = Join-Path $root "strict_bad.sura"
    Set-Content -LiteralPath $strictBad -Encoding UTF8 @"
print "legacy"
"@
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $strictBadOutput = & $enginePath --strict-syntax --check $strictBad 2>&1
    $strictBadCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    $strictBadText = $strictBadOutput -join "`n"
    if ($strictBadCode -eq 0 -or
        $strictBadText -notmatch "legacy command-style syntax is disabled" -or
        $strictBadText -notmatch "strict_bad\.sura") {
        $strictBadOutput | Write-Host
        throw "expected strict syntax to reject legacy command-style calls"
    }

    $shadowWarn = Join-Path $root "shadow_warn.sura"
    Set-Content -LiteralPath $shadowWarn -Encoding UTF8 @"
score is 0
func add_score() do
    score += 1
end
"@
    $shadowOutput = & $enginePath --check $shadowWarn 2>&1
    $shadowCode = $LASTEXITCODE
    $shadowText = $shadowOutput -join "`n"
    if ($shadowCode -ne 0 -or
        $shadowText -notmatch "in-place update 'score \+=' inside a function does not update the top-level variable" -or
        $shadowText -notmatch "Sura check: 1 passed, 0 failed") {
        $shadowOutput | Write-Host
        throw "expected --check to warn about function-local shadowing of top-level state"
    }

    $globalOk = Join-Path $root "global_ok.sura"
    Set-Content -LiteralPath $globalOk -Encoding UTF8 @"
score is 0
func add_score() do
    global score
    score += 1
end
add_score()
assert_eq(score, 1)
"@
    $globalRunOutput = & $enginePath $globalOk 2>&1
    $globalRunCode = $LASTEXITCODE
    if ($globalRunCode -ne 0) {
        $globalRunOutput | Write-Host
        throw "expected global declaration to update top-level state at runtime"
    }
    $globalCheckOutput = & $enginePath --check $globalOk 2>&1
    $globalCheckCode = $LASTEXITCODE
    $globalCheckText = $globalCheckOutput -join "`n"
    if ($globalCheckCode -ne 0 -or
        $globalCheckText -match "does not update the top-level variable" -or
        $globalCheckText -notmatch "Sura check: 1 passed, 0 failed") {
        $globalCheckOutput | Write-Host
        throw "expected global declaration to suppress shadowing warning"
    }

    Write-Host "[PASS] engine check smoke"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
