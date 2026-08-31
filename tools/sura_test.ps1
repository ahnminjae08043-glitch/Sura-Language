param(
    [string]$Path = ".",
    [string]$Engine = "",
    [string]$Report = "sura-test-report.json",
    [string]$JUnit = "",
    [switch]$NoJit,
    [switch]$Recurse,
    [switch]$FailOnSkip,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "sura_test_process.ps1")

function Resolve-SuraEngine {
    param([string]$Explicit)
    if ($Explicit) { return $Explicit }
    if ($env:SURA_ENGINE) { return $env:SURA_ENGINE }
    $root = Split-Path -Parent $PSScriptRoot
    foreach ($candidate in @("SuraLanguage.exe", "SuraEngine.exe", "sura")) {
        $full = Join-Path $root $candidate
        if (Test-Path -LiteralPath $full) { return $full }
    }
    return "SuraLanguage.exe"
}

function Find-SuraTests {
    param([string]$InputPath, [switch]$Recursive)
    if (Test-Path -LiteralPath $InputPath -PathType Leaf) {
        return @(Get-Item -LiteralPath $InputPath)
    }

    $root = Get-Item -LiteralPath $InputPath -ErrorAction SilentlyContinue
    if (-not $root) { return @() }

    $testsDir = Join-Path $root.FullName "tests"
    if (Test-Path -LiteralPath $testsDir -PathType Container) {
        return @(Get-ChildItem -LiteralPath $testsDir -Filter "*.sura" -File -Recurse | Sort-Object FullName)
    }

    $scope = if ($Recursive) { Get-ChildItem -LiteralPath $root.FullName -Filter "*.sura" -File -Recurse }
             else { Get-ChildItem -LiteralPath $root.FullName -Filter "*.sura" -File }
    return @($scope | Where-Object {
        $_.Name -like "test_*.sura" -or $_.Name -like "*_test.sura" -or $_.Name -like "*.test.sura"
    } | Sort-Object FullName)
}

function Write-JUnitReport {
    param(
        [string]$Path,
        [object[]]$Results,
        [int]$Passed,
        [int]$Skipped,
        [int]$Failed,
        [string]$EnginePath,
        [string]$EngineSha256,
        [bool]$Jit
    )

    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }

    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)
    $settings.Indent = $true

    $totalMs = 0
    foreach ($result in $Results) { $totalMs += [int64]$result["durationMs"] }

    $writer = [System.Xml.XmlWriter]::Create($Path, $settings)
    try {
        $writer.WriteStartDocument()
        $writer.WriteStartElement("testsuite")
        $writer.WriteAttributeString("name", "sura")
        $writer.WriteAttributeString("tests", [string]$Results.Count)
        $writer.WriteAttributeString("failures", [string]$Failed)
        $writer.WriteAttributeString("errors", "0")
        $writer.WriteAttributeString("skipped", [string]$Skipped)
        $writer.WriteAttributeString("time", ([string]::Format([System.Globalization.CultureInfo]::InvariantCulture, "{0:0.000}", $totalMs / 1000.0)))

        $writer.WriteStartElement("properties")
        $writer.WriteStartElement("property")
        $writer.WriteAttributeString("name", "engine")
        $writer.WriteAttributeString("value", $EnginePath)
        $writer.WriteEndElement()
        $writer.WriteStartElement("property")
        $writer.WriteAttributeString("name", "engineSha256")
        $writer.WriteAttributeString("value", $EngineSha256)
        $writer.WriteEndElement()
        $writer.WriteStartElement("property")
        $writer.WriteAttributeString("name", "jit")
        $writer.WriteAttributeString("value", $(if ($Jit) { "true" } else { "false" }))
        $writer.WriteEndElement()
        $writer.WriteStartElement("property")
        $writer.WriteAttributeString("name", "passed")
        $writer.WriteAttributeString("value", [string]$Passed)
        $writer.WriteEndElement()
        $writer.WriteStartElement("property")
        $writer.WriteAttributeString("name", "skipped")
        $writer.WriteAttributeString("value", [string]$Skipped)
        $writer.WriteEndElement()
        $writer.WriteEndElement()

        foreach ($result in $Results) {
            $duration = [int64]$result["durationMs"]
            $writer.WriteStartElement("testcase")
            $writer.WriteAttributeString("classname", "sura")
            $writer.WriteAttributeString("name", [string]$result["path"])
            $writer.WriteAttributeString("file", [string]$result["path"])
            $writer.WriteAttributeString("time", ([string]::Format([System.Globalization.CultureInfo]::InvariantCulture, "{0:0.000}", $duration / 1000.0)))
            if ($result["status"] -eq "skip") {
                $writer.WriteStartElement("skipped")
                $writer.WriteAttributeString("message", [string]$result["skipReason"])
                $writer.WriteEndElement()
            } elseif ($result["status"] -ne "pass") {
                $writer.WriteStartElement("failure")
                $failureMessage = if ([bool]$result["timedOut"]) {
                    "timed out after $($result["timeoutSeconds"])s"
                } else {
                    "exit code $($result["exitCode"])"
                }
                $writer.WriteAttributeString("message", $failureMessage)
                $writer.WriteString([string]$result["output"])
                $writer.WriteEndElement()
            }
            if ($result["output"]) {
                $writer.WriteStartElement("system-out")
                $writer.WriteString([string]$result["output"])
                $writer.WriteEndElement()
            }
            $writer.WriteEndElement()
        }

        $writer.WriteEndElement()
        $writer.WriteEndDocument()
    }
    finally {
        $writer.Close()
    }
}

$enginePath = Resolve-SuraEngine $Engine
$tests = Find-SuraTests $Path -Recursive:$Recurse

if (-not $tests -or $tests.Count -eq 0) {
    Write-Host "[error] no Sura tests found under $Path"
    exit 1
}

$engineSnapshot = $null
try {
    $engineSnapshot = New-SuraTestEngineSnapshot -EnginePath $enginePath
} catch {
    Write-Host "[error] could not create an immutable Sura test engine snapshot: $($_.Exception.Message)"
    exit 1
}
$engineSourcePath = $engineSnapshot.SourcePath
$enginePath = $engineSnapshot.Path
Write-Host ("Engine snapshot: {0} ({1} bytes)" -f $engineSnapshot.Sha256, $engineSnapshot.Bytes)

$results = @()
$passed = 0
$skipped = 0
$failed = 0

try {
    foreach ($test in $tests) {
        $args = @()
        if (-not $NoJit) { $args += "--jit" }
        $args += $test.FullName

        $sourceText = [System.IO.File]::ReadAllText($test.FullName)
        $expectMatch = [regex]::Match(
            $sourceText,
            '(?m)^\s*#\s*sura-test:\s*expect-error\s+(\S+)\s*$'
        )
        $expectedError = if ($expectMatch.Success) { $expectMatch.Groups[1].Value } else { "" }

        $run = Invoke-SuraTestProcess -EnginePath $enginePath -Arguments $args -TimeoutSeconds $TimeoutSeconds
        $code = $run.ExitCode

        $relative = Resolve-Path -LiteralPath $test.FullName -Relative
        $outputText = $run.Output
        $passedTest = if ($run.TimedOut) {
            $false
        } elseif ($expectedError) {
            $code -ne 0 -and $outputText -match [regex]::Escape($expectedError)
        } else {
            $code -eq 0
        }
        $skipMatch = if ($passedTest -and -not $expectedError) {
            [regex]::Match($outputText, '(?mi)^\s*[^:\r\n]+:\s*SKIP(?:\s*\((?<reason>[^\r\n]*)\))?\s*$')
        } else {
            [System.Text.RegularExpressions.Match]::Empty
        }
        $skipReason = if ($skipMatch.Success -and $skipMatch.Groups["reason"].Success) {
            $skipMatch.Groups["reason"].Value.Trim()
        } elseif ($skipMatch.Success) {
            "test reported an unmet runtime capability"
        } else {
            ""
        }
        $status = if (-not $passedTest) { "fail" } elseif ($skipMatch.Success) { "skip" } else { "pass" }
        if ($status -eq "pass") {
            $passed++
            Write-Host ("[PASS] {0} ({1} ms)" -f $relative, $run.DurationMs)
        } elseif ($status -eq "skip") {
            $skipped++
            Write-Host ("[SKIP] {0} ({1}; {2} ms)" -f $relative, $skipReason, $run.DurationMs)
        } else {
            $failed++
            if ($run.TimedOut) {
                Write-Host ("[TIMEOUT] {0} exceeded {1}s" -f $relative, $TimeoutSeconds)
            } else {
                Write-Host ("[FAIL] {0} ({1} ms)" -f $relative, $run.DurationMs)
            }
            if ($outputText) { $outputText | Write-Host }
        }

        $results += [ordered]@{
            path = $relative
            status = $status
            exitCode = $code
            timedOut = $run.TimedOut
            timeoutSeconds = $TimeoutSeconds
            expectedError = $expectedError
            skipReason = $skipReason
            durationMs = $run.DurationMs
            output = $outputText
        }
    }
}
finally {
    if (-not (Test-SuraTestEngineSnapshot -Snapshot $engineSnapshot)) {
        $failed++
        $results += [ordered]@{
            path = "[engine snapshot]"
            status = "fail"
            exitCode = 125
            timedOut = $false
            timeoutSeconds = $TimeoutSeconds
            expectedError = ""
            skipReason = ""
            durationMs = 0
            output = "immutable test engine snapshot changed during the suite"
        }
    }
    if (-not (Test-SuraTestEngineSourceUnchanged -Snapshot $engineSnapshot)) {
        Write-Warning "The source Sura engine changed during the suite; results still use snapshot $($engineSnapshot.Sha256)."
    }
    try {
        Remove-SuraTestEngineSnapshot -Snapshot $engineSnapshot
    } catch {
        $failed++
        $results += [ordered]@{
            path = "[engine snapshot cleanup]"
            status = "fail"
            exitCode = 125
            timedOut = $false
            timeoutSeconds = $TimeoutSeconds
            expectedError = ""
            skipReason = ""
            durationMs = 0
            output = "could not remove immutable test engine snapshot: $($_.Exception.Message)"
        }
    }
}

$reportObj = [ordered]@{
    version = 1
    engine = $engineSourcePath
    engineSha256 = $engineSnapshot.Sha256
    engineSnapshot = $true
    jit = (-not $NoJit.IsPresent)
    timeoutSeconds = $TimeoutSeconds
    failOnSkip = $FailOnSkip.IsPresent
    passed = $passed
    skipped = $skipped
    failed = $failed
    tests = $results
}

$reportObj | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Report -Encoding UTF8
Write-Host "Sura tests: $passed passed, $skipped skipped, $failed failed"
Write-Host "[OK] wrote $Report"
if ($JUnit) {
    Write-JUnitReport -Path $JUnit -Results $results -Passed $passed -Skipped $skipped -Failed $failed -EnginePath $engineSourcePath -EngineSha256 $engineSnapshot.Sha256 -Jit:(-not $NoJit.IsPresent)
    Write-Host "[OK] wrote $JUnit"
}

if ($failed -gt 0 -or ($FailOnSkip -and $skipped -gt 0)) { exit 1 }
$global:LASTEXITCODE = 0
