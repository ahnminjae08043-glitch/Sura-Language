param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([IO.Path]::GetTempPath()) ("sura_gc_stats_" + [Guid]::NewGuid().ToString("N"))
$utf8 = New-Object Text.UTF8Encoding($false)

function Run-Engine([string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $enginePath @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Text = ($output -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $program = Join-Path $temp "allocation_pressure.sura"
    $jsonPath = Join-Path $temp "gc-stats.json"
    [IO.File]::WriteAllText($program, @"
current is nil
repeat 12050 do
    current is [1, 2, 3]
end
print("gc-stats-program-ok")
"@, $utf8)

    $jsonRun = Run-Engine @("--gc-stats-json", $jsonPath, $program)
    if ($jsonRun.Code -ne 0 -or $jsonRun.Text -notmatch 'gc-stats-program-ok' -or
        $jsonRun.Text -notmatch '\[gc\] wrote') {
        Write-Output $jsonRun.Text
        throw "GC JSON statistics execution failed"
    }
    if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "GC statistics JSON was not written"
    }

    $stats = Get-Content -LiteralPath $jsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($stats.schema -ne "sura.gc_stats.v1" -or
        [int64]$stats.collections -lt 2 -or
        [int64]$stats.objects_reclaimed -lt 1000 -or
        [int64]$stats.peak_objects -lt 1000 -or
        [int64]$stats.last_objects_before -lt [int64]$stats.last_objects_after -or
        [int64]$stats.next_object_threshold -lt 1024 -or
        [double]$stats.average_pause_us -lt 0) {
        throw "GC statistics JSON did not satisfy the runtime contract: $($stats | ConvertTo-Json -Compress)"
    }

    $textRun = Run-Engine @("--gc-stats", $program)
    if ($textRun.Code -ne 0 -or
        $textRun.Text -notmatch '=== Sura GC Statistics ===' -or
        $textRun.Text -notmatch 'Objects reclaimed:' -or
        $textRun.Text -notmatch 'Maximum pause:' -or
        $textRun.Text -notmatch 'Next object threshold:') {
        Write-Output $textRun.Text
        throw "GC text statistics execution failed"
    }

    Write-Host "sura_gc_stats_smoke: PASS (collections=$($stats.collections), reclaimed=$($stats.objects_reclaimed), max_pause_us=$($stats.max_pause_us))"
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
