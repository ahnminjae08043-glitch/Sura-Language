<#
.SYNOPSIS
    Compare two Sura engine builds on the same benchmark, with interleaved runs.

.DESCRIPTION
    sura_bench_now.ps1 measures one engine and compares it against recorded
    history. That answers "is this build slower than last time", but it cannot
    validate an optimization: machine noise on this class of hardware routinely
    moves a single measurement by 30% or more. The same unmodified binary has
    been observed at both 103 ms and 138 ms on bench_fib.sura minutes apart.

    This script removes that drift by running A and B back to back inside every
    round, so both builds see the same thermal and background-load conditions.
    Two statistics are reported:

      * minimum  - the least noise-contaminated sample, used as the headline.
                   Noise only ever adds time, so the floor is the honest number.
      * win rate - how many rounds B beat A. A real improvement wins nearly
                   every round; noise splits roughly evenly.

    A change that improves the minimum but wins only half its rounds has not
    been demonstrated. Report both numbers, not just the one that looks better.

.PARAMETER EngineA
    Baseline engine executable.

.PARAMETER EngineB
    Candidate engine executable to compare against the baseline.

.PARAMETER Script
    Benchmark .sura file. It must print a line matching "avg (N runs): X ms".

.PARAMETER Rounds
    Interleaved A/B rounds. Fewer than 5 makes the win rate meaningless.

.PARAMETER Jit
    Run both engines with `--jit`. Required to measure anything about the
    native JIT: without it both sides execute on the register VM, so a codegen
    change shows up as noise. Check the engine's `[JIT-OPS]` line to confirm the
    workload actually reaches the emitters you changed.

.PARAMETER FailIfSlowerPercent
    If set, exit 1 when B's minimum is more than this percentage slower than A's.
    Use in CI to gate performance regressions.

.EXAMPLE
    .\tools\sura_ab_bench.ps1 -EngineA .\SuraLanguage.exe -EngineB .\SuraNew.exe -Script bench_fib.sura

.EXAMPLE
    .\tools\sura_ab_bench.ps1 -EngineA old.exe -EngineB new.exe -Script bench_fib.sura -Rounds 12 -FailIfSlowerPercent 2
#>
param(
    [Parameter(Mandatory = $true)][string]$EngineA,
    [Parameter(Mandatory = $true)][string]$EngineB,
    [Parameter(Mandatory = $true)][string]$Script,
    [ValidateRange(1, 200)][int]$Rounds = 8,
    [string]$LabelA = "A",
    [string]$LabelB = "B",
    [switch]$Jit,
    [ValidateRange(0, 1000)][double]$FailIfSlowerPercent = -1,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 300
)

$ErrorActionPreference = "Stop"

foreach ($pathInfo in @(@{ Name = "EngineA"; Path = $EngineA },
                        @{ Name = "EngineB"; Path = $EngineB },
                        @{ Name = "Script";  Path = $Script })) {
    if (-not (Test-Path -LiteralPath $pathInfo.Path)) {
        Write-Error ("{0} not found: {1}" -f $pathInfo.Name, $pathInfo.Path)
        exit 2
    }
}

# Parse the "avg (5 runs): 103.77 ms" line the bench_*.sura scripts emit.
function Get-BenchMilliseconds {
    param([string]$Engine, [string]$BenchScript, [int]$Timeout, [bool]$UseJit)

    # Capture the redirect paths before launching: ProcessStartInfo exposes
    # RedirectStandardOutput as a bool, so they cannot be recovered from $proc.
    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    $argList = @()
    if ($UseJit) { $argList += "--jit" }
    $argList += $BenchScript
    $proc = Start-Process -FilePath $Engine -ArgumentList $argList `
                          -NoNewWindow -PassThru `
                          -RedirectStandardOutput $outFile `
                          -RedirectStandardError $errFile
    # Dereferencing Handle caches it in the object. Without this, .NET releases
    # the handle when the process exits and ExitCode comes back $null.
    $null = $proc.Handle
    try {
        if (-not $proc.WaitForExit($Timeout * 1000)) {
            try { $proc.Kill() } catch {}
            throw ("Engine timed out after {0}s: {1}" -f $Timeout, $Engine)
        }
        # The timed overload can return before ExitCode and the redirected
        # streams have settled, leaving ExitCode $null. The argument-less call
        # completes that processing.
        $proc.WaitForExit()
        $exitCode = $proc.ExitCode
        $text = ""
        if (Test-Path -LiteralPath $outFile) { $text = [System.IO.File]::ReadAllText($outFile) }
        if ($exitCode -ne 0) {
            $errText = ""
            if (Test-Path -LiteralPath $errFile) { $errText = [System.IO.File]::ReadAllText($errFile) }
            throw ("Engine exited {0}: {1}`n{2}" -f $exitCode, $Engine, $errText)
        }
        $match = [regex]::Match($text, 'avg \([0-9]+ runs\):\s*([0-9.]+)\s*ms')
        if (-not $match.Success) {
            throw ("No 'avg (N runs): X ms' line in output of {0}. The benchmark script must print one." -f $Engine)
        }
        return [double]$match.Groups[1].Value
    } finally {
        foreach ($f in @($outFile, $errFile)) {
            if ($f -and (Test-Path -LiteralPath $f)) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
        }
    }
}

Write-Host ("Benchmark : {0}" -f $Script)
Write-Host ("{0,-9}: {1}" -f $LabelA, $EngineA)
Write-Host ("{0,-9}: {1}" -f $LabelB, $EngineB)
Write-Host ("Rounds    : {0} (interleaved)" -f $Rounds)
# Say which engine mode was measured. A JIT codegen change compared without
# --jit runs entirely on the register VM and reads as noise, so this line is
# what stops a result from being quietly misinterpreted later.
Write-Host ("Mode      : {0}" -f $(if ($Jit) { "--jit (native JIT enabled)" } else { "register VM only (no --jit)" }))
Write-Host ""

$samplesA = New-Object System.Collections.Generic.List[double]
$samplesB = New-Object System.Collections.Generic.List[double]
$winsB = 0

# Discarded warm-up. The first process launch of a session pays for cold file
# cache and an unramped CPU, and has been observed 30%+ above the steady state.
# Whichever build happened to go first would otherwise absorb that penalty.
Write-Host "warm-up (discarded)"
$null = Get-BenchMilliseconds -Engine $EngineA -BenchScript $Script -Timeout $TimeoutSeconds -UseJit $Jit
$null = Get-BenchMilliseconds -Engine $EngineB -BenchScript $Script -Timeout $TimeoutSeconds -UseJit $Jit

for ($i = 1; $i -le $Rounds; $i++) {
    # Interleaved within the round so both builds share the same conditions,
    # and the order alternates (ABBA) so that neither build is systematically
    # the later - and therefore hotter - of the pair. Under thermal drift a
    # fixed A-then-B order biases every round against B.
    if ($i % 2 -eq 1) {
        $a = Get-BenchMilliseconds -Engine $EngineA -BenchScript $Script -Timeout $TimeoutSeconds -UseJit $Jit
        $b = Get-BenchMilliseconds -Engine $EngineB -BenchScript $Script -Timeout $TimeoutSeconds -UseJit $Jit
    } else {
        $b = Get-BenchMilliseconds -Engine $EngineB -BenchScript $Script -Timeout $TimeoutSeconds -UseJit $Jit
        $a = Get-BenchMilliseconds -Engine $EngineA -BenchScript $Script -Timeout $TimeoutSeconds -UseJit $Jit
    }
    $samplesA.Add($a)
    $samplesB.Add($b)
    if ($b -lt $a) { $winsB++ }
    $delta = if ($a -gt 0) { (($a - $b) / $a) * 100.0 } else { 0.0 }
    Write-Host ("round {0,3}   {1} = {2,9:F2} ms   {3} = {4,9:F2} ms   {5,6:F1}%" -f `
                $i, $LabelA, $a, $LabelB, $b, $delta)
}

$minA = ($samplesA | Measure-Object -Minimum).Minimum
$minB = ($samplesB | Measure-Object -Minimum).Minimum
$improvement = if ($minA -gt 0) { (($minA - $minB) / $minA) * 100.0 } else { 0.0 }

Write-Host ""
Write-Host "=== Result (minimum of $Rounds rounds) ==="
Write-Host ("{0,-9} min : {1,9:F2} ms" -f $LabelA, $minA)
Write-Host ("{0,-9} min : {1,9:F2} ms" -f $LabelB, $minB)
Write-Host ("Change      : {0,8:F1}%  ({1})" -f $improvement, $(if ($improvement -ge 0) { "$LabelB faster" } else { "$LabelB slower" }))
Write-Host ("Win rate    : {0}/{1} rounds to {2}" -f $winsB, $Rounds, $LabelB)

# A genuine change wins nearly every round. An even split is noise, whatever
# the minimum happens to say, so say that out loud rather than let the
# headline percentage stand unqualified.
#
# Below MIN_VERDICT_ROUNDS a clean sweep proves nothing: running the same
# binary against itself for 3 rounds produces a 3/3 "win" one time in eight.
# Refuse to call those, rather than hand back a confident-looking verdict that
# a coin flip would reproduce.
$MIN_VERDICT_ROUNDS = 8
$winRate = $winsB / [double]$Rounds
if ($Rounds -lt $MIN_VERDICT_ROUNDS) {
    Write-Host ("Verdict     : NOT ENOUGH ROUNDS - {0} run, {1} needed before a sweep means anything." -f `
                $Rounds, $MIN_VERDICT_ROUNDS)
} elseif ($winRate -ge 0.8) {
    Write-Host "Verdict     : consistent - $LabelB wins the large majority of rounds."
} elseif ($winRate -le 0.2) {
    Write-Host "Verdict     : consistent - $LabelA wins the large majority of rounds."
} else {
    Write-Host "Verdict     : INCONCLUSIVE - rounds are split; this is within noise."
}

if ($FailIfSlowerPercent -ge 0 -and $improvement -lt (-1 * $FailIfSlowerPercent)) {
    Write-Error ("Regression: {0} is {1:F1}% slower than {2} (threshold {3:F1}%)." -f `
                 $LabelB, (-1 * $improvement), $LabelA, $FailIfSlowerPercent)
    exit 1
}
exit 0
