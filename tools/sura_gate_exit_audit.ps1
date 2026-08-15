<#
.SYNOPSIS
    Find gates whose printed verdict and exit code can disagree.

.DESCRIPTION
    A PowerShell script with no explicit `exit` returns whatever its last
    command returned. For a gate that ends by printing "PASS", that means the
    human-readable verdict and the machine-readable one are produced by two
    different things - and CI acts on the second.

    This is not hypothetical. `sura_build_contract_smoke.ps1` ended its run by
    deliberately invoking `build.bat` with an invalid mode to check that it is
    rejected. `build.bat` exits 2 for that, correctly. With no `exit 0` after
    the PASS line, the gate inherited the 2: it printed PASS and exited 2, so
    CI saw a failing gate whose output said it passed.

    Most gates get away with it because their last command happens to return 0.
    "Happens to" is the problem: a gate should state its result rather than
    inherit one.

    This audit is static - it reads scripts rather than running them, so it is
    fast enough to sit in front of a commit. It cannot tell which gates would
    actually mismatch at runtime; it reports which ones leave it to chance.

.PARAMETER Path
    Directory to scan. Defaults to tools/.

.PARAMETER Filter
    Filename pattern. Defaults to gate and smoke scripts.

.PARAMETER FailOnFinding
    Exit 1 when any script lacks an explicit exit. Off by default, because the
    existing tree has many and fixing them is a separate decision.

.EXAMPLE
    .\tools\sura_gate_exit_audit.ps1

.EXAMPLE
    .\tools\sura_gate_exit_audit.ps1 -FailOnFinding
#>
param(
    [string]$Path = "",
    [string[]]$Filter = @("*gate*.ps1", "*smoke*.ps1"),
    [switch]$FailOnFinding
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
if (-not $Path) { $Path = Join-Path $root "tools" }
if (-not (Test-Path -LiteralPath $Path)) {
    Write-Error "Path not found: $Path"
    exit 2
}

$files = @()
foreach ($f in $Filter) {
    $files += Get-ChildItem -LiteralPath $Path -Filter $f -File -ErrorAction SilentlyContinue
}
$files = $files | Sort-Object -Property FullName -Unique
if ($files.Count -eq 0) {
    Write-Host "No gate or smoke scripts found under $Path."
    exit 0
}

$explicit = New-Object System.Collections.Generic.List[string]
$implicit = New-Object System.Collections.Generic.List[string]

foreach ($file in $files) {
    $text = Get-Content -LiteralPath $file.FullName -Raw

    # Only the tail matters: an `exit` inside an early-return guard does not
    # decide the success path's code.
    $tail = ($text -split "`r?`n" | Where-Object { $_.Trim() -ne "" } | Select-Object -Last 6) -join "`n"
    if ($tail -match '(?m)^\s*exit\s+') {
        $explicit.Add($file.Name)
    } else {
        $implicit.Add($file.Name)
    }
}

Write-Host ("Scanned : {0} gate/smoke scripts under {1}" -f $files.Count, ($Path.Replace($root, "").TrimStart("\", "/")))
Write-Host ("Explicit: {0} end with an explicit exit" -f $explicit.Count)
Write-Host ("Implicit: {0} inherit their exit code from the last command" -f $implicit.Count)
Write-Host ""

if ($implicit.Count -gt 0) {
    Write-Host "Scripts whose exit code is left to the last command:"
    foreach ($n in $implicit) { Write-Host ("  {0}" -f $n) }
    Write-Host ""
    Write-Host "This is only a defect where the last command can return nonzero on the"
    Write-Host "success path - a gate that finishes by checking something is correctly"
    Write-Host "rejected is the case that bites. Add `exit 0` after the PASS line."
}

if ($FailOnFinding -and $implicit.Count -gt 0) {
    Write-Error ("{0} gate script(s) do not state their exit code." -f $implicit.Count)
    exit 1
}
exit 0
