param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$fixture = Join-Path $PSScriptRoot "fixtures\distributed_autograd_worker.sura"
$rejectFixture = Join-Path $PSScriptRoot "fixtures\distributed_autograd_reject_worker.sura"
if (-not (Test-Path -LiteralPath $fixture -PathType Leaf)) {
    throw "distributed worker fixture is missing: $fixture"
}
if (-not (Test-Path -LiteralPath $rejectFixture -PathType Leaf)) {
    throw "distributed reject fixture is missing: $rejectFixture"
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$temp = Join-Path $tempRoot ("sura_distributed_autograd_" + [System.Guid]::NewGuid().ToString("N"))
$stdoutFiles = New-Object System.Collections.Generic.List[string]
$stderrFiles = New-Object System.Collections.Generic.List[string]

function Quote-NativeArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashes += 1
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * ($slashes * 2 + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) {
            [void]$builder.Append(('\' * $slashes))
            $slashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) {
        [void]$builder.Append(('\' * ($slashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Start-SuraWorker {
    param(
        [string]$Mode,
        [string]$Rendezvous,
        [string]$RunId,
        [int]$Step,
        [int]$Rank,
        [int]$WorldSize,
        [bool]$Average,
        [string]$WorkerFixture = $fixture
    )

    $arguments = New-Object System.Collections.Generic.List[string]
    if ($Mode -eq "jit") {
        $arguments.Add("--jit")
    }
    $arguments.Add($WorkerFixture)
    $arguments.Add("--")
    $arguments.Add($Rendezvous)
    $arguments.Add($RunId)
    $arguments.Add([string]$Step)
    $arguments.Add([string]$Rank)
    $arguments.Add([string]$WorldSize)
    $arguments.Add($(if ($Average) { "true" } else { "false" }))

    $stdout = Join-Path $temp ("$Mode-rank-$Rank.stdout.txt")
    $stderr = Join-Path $temp ("$Mode-rank-$Rank.stderr.txt")
    $stdoutFiles.Add($stdout)
    $stderrFiles.Add($stderr)

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $enginePath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    if ($null -ne $start.ArgumentList) {
        foreach ($argument in $arguments) {
            $start.ArgumentList.Add($argument)
        }
    } else {
        $start.Arguments = (($arguments | ForEach-Object { Quote-NativeArgument $_ }) -join " ")
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    if (-not $process.Start()) {
        throw "failed to start distributed worker rank $Rank"
    }
    return [pscustomobject]@{
        Process = $process
        Stdout = $stdout
        Stderr = $stderr
        Rank = $Rank
    }
}

function Wait-SuraWorkers {
    param(
        [array]$Workers,
        [int]$TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        $running = @($Workers | Where-Object { -not $_.Process.HasExited })
        if ($running.Count -eq 0) {
            break
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            foreach ($worker in $running) {
                try { $worker.Process.Kill() } catch {}
            }
            throw "distributed workers timed out"
        }
        Start-Sleep -Milliseconds 50
    }

    foreach ($worker in $Workers) {
        $stdout = $worker.Process.StandardOutput.ReadToEnd()
        $stderr = $worker.Process.StandardError.ReadToEnd()
        [System.IO.File]::WriteAllText($worker.Stdout, $stdout)
        [System.IO.File]::WriteAllText($worker.Stderr, $stderr)
        if ($worker.Process.ExitCode -ne 0 -or $stdout -notmatch "distributed_worker_ok") {
            Write-Output "rank $($worker.Rank) stdout:"
            Write-Output $stdout
            Write-Output "rank $($worker.Rank) stderr:"
            Write-Output $stderr
            throw "distributed worker rank $($worker.Rank) failed with exit $($worker.Process.ExitCode)"
        }
        $worker.Process.Dispose()
    }
}

try {
    New-Item -ItemType Directory -Path $temp | Out-Null
    $rendezvous = Join-Path $temp "rendezvous"

    foreach ($mode in @("vm", "jit")) {
        foreach ($average in @($true, $false)) {
            $runId = "smoke-$mode-$average-" + [System.Guid]::NewGuid().ToString("N")
            $workers = @(
                (Start-SuraWorker -Mode $mode -Rendezvous $rendezvous -RunId $runId -Step 7 -Rank 0 -WorldSize 2 -Average $average),
                (Start-SuraWorker -Mode $mode -Rendezvous $rendezvous -RunId $runId -Step 7 -Rank 1 -WorldSize 2 -Average $average)
            )
            Wait-SuraWorkers -Workers $workers
        }
    }

    # world_size=1 must remain a validated filesystem-free no-op.
    $single = Start-SuraWorker -Mode "vm" -Rendezvous $rendezvous `
        -RunId ("single-" + [System.Guid]::NewGuid().ToString("N")) `
        -Step 0 -Rank 0 -WorldSize 1 -Average $true
    Wait-SuraWorkers -Workers @($single)

    # Publish a valid rank-1 file, stop that worker, then corrupt its SHA-256
    # footer. Rank 0 must reject the operation without changing either local
    # gradient buffer.
    $corruptRun = "corrupt-" + [System.Guid]::NewGuid().ToString("N")
    $rankOne = Start-SuraWorker -Mode "vm" -Rendezvous $rendezvous `
        -RunId $corruptRun -Step 9 -Rank 1 -WorldSize 2 -Average $true
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $runBytes = [System.Text.Encoding]::UTF8.GetBytes($corruptRun)
        $runHash = -join ($sha.ComputeHash($runBytes) | ForEach-Object { $_.ToString("x2") })
    } finally {
        $sha.Dispose()
    }
    $rankOnePath = Join-Path (Join-Path (Join-Path $rendezvous "run-$runHash") "step-9") "rank-1.sgrad"
    $publishDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $rankOnePath -PathType Leaf)) {
        if ($rankOne.Process.HasExited) {
            $stdout = $rankOne.Process.StandardOutput.ReadToEnd()
            $stderr = $rankOne.Process.StandardError.ReadToEnd()
            throw "rank 1 exited before publishing corruption fixture: $stdout $stderr"
        }
        if ([DateTime]::UtcNow -ge $publishDeadline) {
            try { $rankOne.Process.Kill() } catch {}
            throw "rank 1 did not publish corruption fixture"
        }
        Start-Sleep -Milliseconds 20
    }
    try { $rankOne.Process.Kill() } catch {}
    [void]$rankOne.Process.WaitForExit(5000)
    [void]$rankOne.Process.StandardOutput.ReadToEnd()
    [void]$rankOne.Process.StandardError.ReadToEnd()
    $rankOne.Process.Dispose()

    $stream = [System.IO.File]::Open(
        $rankOnePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    try {
        [void]$stream.Seek(-1, [System.IO.SeekOrigin]::End)
        $last = $stream.ReadByte()
        [void]$stream.Seek(-1, [System.IO.SeekOrigin]::End)
        $stream.WriteByte($last -bxor 1)
        $stream.Flush()
    } finally {
        $stream.Dispose()
    }

    $reject = Start-SuraWorker -Mode "vm" -Rendezvous $rendezvous `
        -RunId $corruptRun -Step 9 -Rank 0 -WorldSize 2 -Average $true `
        -WorkerFixture $rejectFixture
    Wait-SuraWorkers -Workers @($reject)

    "sura_distributed_autograd_smoke: PASS"
}
finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    if ($resolvedTemp.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) `
        -and (Split-Path -Leaf $resolvedTemp).StartsWith("sura_distributed_autograd_")) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
