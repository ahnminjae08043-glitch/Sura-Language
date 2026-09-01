<#
.SYNOPSIS
    Differential test: the native JIT must produce exactly what the register VM produces.

.DESCRIPTION
    jit_native.hpp emits x86-64/ARM64 machine code directly. When an emitter is
    wrong it does not usually crash - it computes a different number, and the
    program keeps running. run_stable_tests.ps1 cannot catch that class of bug,
    because it only runs one engine configuration and checks that the test says
    PASS. A miscompiled program that still prints PASS looks identical to a
    correct one.

    This script runs each program twice, once with the native JIT enabled
    (`--jit`) and once on the register VM alone, and requires the output to
    match byte for byte. Any divergence is evidence of a codegen defect, and
    the register VM is the reference because it is the interpreter the language
    is specified by.

    Two design points keep the result honest:

    * Nondeterministic programs are detected, not guessed at. Anything using
      clock(), random_*, or uuid() prints different text on every run, which
      would look like a JIT divergence. So each program is first run twice on
      the VM alone. If those two runs already disagree, the program is reported
      NONDET and excluded - it cannot be used as a differential oracle.

    * A program that never triggers native compilation is not a pass. The
      engine reports "[JIT] N function(s), M method(s) compiled to native".
      When N and M are both zero the run exercised no emitter at all, so it is
      reported NO-NATIVE rather than counted as evidence. A suite that is
      entirely NO-NATIVE proves nothing about the JIT, and the summary says so.

.PARAMETER Engine
    Engine executable. Defaults to .\SuraLanguage.exe.

.PARAMETER Path
    Directory to scan for .sura files, or one or more explicit files.

.PARAMETER TimeoutSeconds
    Per-run timeout.

.PARAMETER FailOnNoNative
    Exit non-zero if no program in the set exercised native compilation.
    Use this in a JIT certification lane.

.EXAMPLE
    .\tools\sura_jit_differential.ps1 -Path tests

.EXAMPLE
    .\tools\sura_jit_differential.ps1 -Path tests -Engine .\SuraLanguage.exe -FailOnNoNative
#>
param(
    [string]$Engine = "",
    [string[]]$Path = @("tests"),
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 120,
    [switch]$FailOnNoNative
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
if (-not $Engine) { $Engine = Join-Path $root "SuraLanguage.exe" }
if (-not (Test-Path -LiteralPath $Engine)) {
    Write-Error "Engine not found: $Engine. Run build.bat first."
    exit 2
}

# Collect the program set.
$files = @()
foreach ($p in $Path) {
    $full = if ([System.IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p }
    if (Test-Path -LiteralPath $full -PathType Container) {
        $files += Get-ChildItem -LiteralPath $full -Filter "*.sura" -File | Sort-Object Name
    } elseif (Test-Path -LiteralPath $full) {
        $files += Get-Item -LiteralPath $full
    } else {
        Write-Error "Path not found: $p"
        exit 2
    }
}
if ($files.Count -eq 0) { Write-Error "No .sura files found."; exit 2 }

# The engine prints its own "[JIT] ..." summary only in --jit mode, so it must
# be removed before comparing, along with the blank line it is preceded by.
function Remove-JitBanner {
    param([string]$Text)
    # Every engine-emitted JIT report line, not just the ones that existed when
    # this was written: they appear only in the --jit run, so leaving one
    # unfiltered makes every program look like a divergence.
    $lines = $Text -split "`r?`n" | Where-Object { $_ -notmatch '^\[JIT[A-Z-]*\]\s' }
    return ($lines -join "`n").TrimEnd()
}

# "[JIT-OPS] 7 opcode(s) emitted: ADD SUB MOVE ..." - which emitters this
# program actually reached. Aggregated across the run so the summary can name
# the covered set rather than only counting programs.
function Get-EmittedOps {
    param([string]$Text)
    $m = [regex]::Match($Text, '\[JIT-OPS\]\s+\d+\s+opcode\(s\) emitted:(.*)')
    if (-not $m.Success) { return @() }
    $rest = $m.Groups[1].Value.Trim()
    if ($rest -eq "" -or $rest -eq "(none)") { return @() }
    return $rest -split '\s+' | Where-Object { $_ -ne "" }
}

function Invoke-Engine {
    param([string]$EnginePath, [string]$File, [switch]$Jit, [int]$Timeout)

    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    $argList = @()
    if ($Jit) { $argList += "--jit" }
    $argList += $File
    $proc = Start-Process -FilePath $EnginePath -ArgumentList $argList `
                          -NoNewWindow -PassThru `
                          -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $null = $proc.Handle   # cache the handle or ExitCode comes back $null
    try {
        if (-not $proc.WaitForExit($Timeout * 1000)) {
            try { $proc.Kill() } catch {}
            return @{ TimedOut = $true; Text = ""; ExitCode = -1 }
        }
        $proc.WaitForExit()
        $text = ""
        if (Test-Path -LiteralPath $outFile) { $text = [System.IO.File]::ReadAllText($outFile) }
        return @{ TimedOut = $false; Text = $text; ExitCode = $proc.ExitCode }
    } finally {
        foreach ($f in @($outFile, $errFile)) {
            if ($f -and (Test-Path -LiteralPath $f)) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
        }
    }
}

$match = 0; $diverged = 0; $nondet = 0; $noNative = 0; $errored = 0
$nativeExercised = 0
$divergedFiles = @()
$allEmittedOps = New-Object System.Collections.Generic.HashSet[string]
# opcode -> how many programs it disqualified. The top level compiles
# all-or-nothing, so one rejected opcode costs a whole program its native code.
# Ranking them is how DIV and USE_LIB were found, each worth roughly 6x once
# given an emitter.
$bailCounts = @{}

Write-Host ("Engine : {0}" -f $Engine)
Write-Host ("Programs: {0}" -f $files.Count)
Write-Host ""

foreach ($f in $files) {
    $rel = $f.FullName.Replace($root, "").TrimStart("\", "/")

    # Reference run on the register VM.
    $vm1 = Invoke-Engine -EnginePath $Engine -File $f.FullName -Timeout $TimeoutSeconds
    if ($vm1.TimedOut) { Write-Host ("[ERROR   ] {0} (VM timeout)" -f $rel); $errored++; continue }

    # Second VM run: detects nondeterministic output before it can be mistaken
    # for a JIT divergence.
    $vm2 = Invoke-Engine -EnginePath $Engine -File $f.FullName -Timeout $TimeoutSeconds
    if ($vm2.TimedOut) { Write-Host ("[ERROR   ] {0} (VM timeout)" -f $rel); $errored++; continue }

    $vmText1 = Remove-JitBanner $vm1.Text
    $vmText2 = Remove-JitBanner $vm2.Text
    if ($vmText1 -ne $vmText2) {
        Write-Host ("[NONDET  ] {0} (output varies between identical VM runs)" -f $rel)
        $nondet++
        continue
    }

    # JIT run.
    $jit = Invoke-Engine -EnginePath $Engine -File $f.FullName -Jit -Timeout $TimeoutSeconds
    if ($jit.TimedOut) { Write-Host ("[ERROR   ] {0} (JIT timeout)" -f $rel); $errored++; continue }
    $jitText = Remove-JitBanner $jit.Text

    # Did this program actually compile anything to native code? Count emitted
    # opcodes rather than the "[JIT] N function(s), M method(s)" tally: that
    # tally comes from native_funcs/native_methods and excludes the top-level
    # main chunk, so a program where only main compiled would otherwise be
    # filed under "no native compilation" while native code was in fact run.
    $emittedOps = Get-EmittedOps $jit.Text
    $compiled = $emittedOps.Count
    if ($jit.Text -notmatch '\[JIT-OPS\]') {
        # Engine predates the [JIT-OPS] report. Fall back to the coarser tally
        # so an older build is not reported as compiling nothing at all.
        $m = [regex]::Match($jit.Text, '\[JIT\]\s+(\d+)\s+function\(s\),\s+(\d+)\s+method\(s\)')
        if ($m.Success) { $compiled = [int]$m.Groups[1].Value + [int]$m.Groups[2].Value }
    }

    if ($jitText -ne $vmText1 -or $jit.ExitCode -ne $vm1.ExitCode) {
        Write-Host ("[DIVERGED] {0} (exit {1} vs {2}, {3} native callable(s))" -f `
                    $rel, $vm1.ExitCode, $jit.ExitCode, $compiled)
        $diverged++
        $divergedFiles += $rel
        continue
    }

    $bail = [regex]::Match($jit.Text, '\[JIT-BAIL\] top level not compiled: (\S+) at ip=')
    if ($bail.Success) {
        $op = $bail.Groups[1].Value
        if ($bailCounts.ContainsKey($op)) { $bailCounts[$op]++ } else { $bailCounts[$op] = 1 }
    }

    if ($compiled -eq 0) {
        # Outputs agree, but no emitter ran, so this says nothing about codegen.
        $noNative++
    } else {
        $nativeExercised++
        $match++
        foreach ($op in $emittedOps) { $null = $allEmittedOps.Add($op) }
    }
}

Write-Host ""
Write-Host "=== JIT differential summary ==="
Write-Host ("Verified (native code ran, output matched VM) : {0}" -f $match)
Write-Host ("DIVERGED (native output differs from VM)      : {0}" -f $diverged)
Write-Host ("No native compilation (proves nothing)        : {0}" -f $noNative)
Write-Host ("Nondeterministic (excluded)                   : {0}" -f $nondet)
Write-Host ("Errors / timeouts                             : {0}" -f $errored)

# Naming the covered emitters turns "N programs used the JIT" into something
# actionable: compare this set against the switch in jit_native.hpp's emit_op
# to see which emitters still have nothing checking them.
if ($allEmittedOps.Count -gt 0) {
    $sorted = $allEmittedOps | Sort-Object
    Write-Host ""
    Write-Host ("Emitters exercised ({0} opcodes):" -f $allEmittedOps.Count)
    Write-Host ("  {0}" -f ($sorted -join " "))
} elseif ($nativeExercised -gt 0) {
    Write-Host ""
    Write-Host "Emitters exercised: unknown (engine did not report a [JIT-OPS] line)."
}

# Which opcodes are keeping programs out of the JIT, most costly first. Each
# one listed here disqualified that many whole top levels, so this is the
# ranked worklist for what to give an emitter next.
if ($bailCounts.Count -gt 0) {
    Write-Host ""
    Write-Host "Top-level compiles blocked by opcode (programs disqualified):"
    $bailCounts.GetEnumerator() | Sort-Object -Property Value -Descending | ForEach-Object {
        Write-Host ("  {0,-16} {1}" -f $_.Key, $_.Value)
    }
}

if ($diverged -gt 0) {
    Write-Host ""
    Write-Host "Diverged programs:"
    $divergedFiles | ForEach-Object { Write-Host ("  {0}" -f $_) }
    Write-Error "Native JIT output does not match the register VM. This is a codegen defect."
    exit 1
}

if ($nativeExercised -eq 0) {
    Write-Host ""
    Write-Host "WARNING: no program in this set compiled anything to native code."
    Write-Host "         This run is not evidence that the JIT is correct."
    if ($FailOnNoNative) { exit 1 }
}

if ($errored -gt 0) { exit 1 }
exit 0
