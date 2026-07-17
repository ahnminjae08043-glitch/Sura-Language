function Resolve-SuraTestEngineFile {
    param([Parameter(Mandatory=$true)][string]$EnginePath)

    if (Test-Path -LiteralPath $EnginePath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $EnginePath).Path
    }
    $command = Get-Command $EnginePath -ErrorAction SilentlyContinue
    if ($command -and $command.Source -and (Test-Path -LiteralPath $command.Source -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $command.Source).Path
    }
    throw "Sura engine not found: $EnginePath"
}

function New-SuraTestEngineSnapshot {
    param([Parameter(Mandatory=$true)][string]$EnginePath)

    $sourcePath = Resolve-SuraTestEngineFile $EnginePath
    $snapshotRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_test_engine_" + [System.Guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Force -Path $snapshotRoot | Out-Null
        $snapshotPath = Join-Path $snapshotRoot ([System.IO.Path]::GetFileName($sourcePath))
        Copy-Item -LiteralPath $sourcePath -Destination $snapshotPath -Force

        # Keep dynamically loaded runtime files beside the immutable executable.
        # This covers MinGW DLLs on Windows and sibling shared libraries on
        # Linux/macOS without copying unrelated source or test files.
        $sourceDirectory = Split-Path -Parent $sourcePath
        foreach ($runtimeFile in @(Get-ChildItem -LiteralPath $sourceDirectory -File -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -match '(?i)\.dll$|\.dylib$|\.so(?:\.|$)'
        })) {
            Copy-Item -LiteralPath $runtimeFile.FullName -Destination (Join-Path $snapshotRoot $runtimeFile.Name) -Force
        }

        $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $snapshotHash = (Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($sourceHash -ne $snapshotHash) {
            throw "Sura engine snapshot hash mismatch"
        }

        return [pscustomobject]@{
            SourcePath = $sourcePath
            Path = $snapshotPath
            Root = $snapshotRoot
            Sha256 = $snapshotHash
            Bytes = [int64](Get-Item -LiteralPath $snapshotPath).Length
        }
    }
    catch {
        if (Test-Path -LiteralPath $snapshotRoot) {
            Remove-Item -LiteralPath $snapshotRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}

function Test-SuraTestEngineSnapshot {
    param([Parameter(Mandatory=$true)][object]$Snapshot)

    if (-not (Test-Path -LiteralPath $Snapshot.Path -PathType Leaf)) { return $false }
    try {
        $actual = (Get-FileHash -LiteralPath $Snapshot.Path -Algorithm SHA256).Hash.ToLowerInvariant()
        return $actual -eq [string]$Snapshot.Sha256
    }
    catch {
        return $false
    }
}

function Test-SuraTestEngineSourceUnchanged {
    param([Parameter(Mandatory=$true)][object]$Snapshot)

    if (-not (Test-Path -LiteralPath $Snapshot.SourcePath -PathType Leaf)) { return $false }
    try {
        $actual = (Get-FileHash -LiteralPath $Snapshot.SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        return $actual -eq [string]$Snapshot.Sha256
    }
    catch {
        return $false
    }
}

function Remove-SuraTestEngineSnapshot {
    param([Parameter(Mandatory=$true)][object]$Snapshot)

    if (-not $Snapshot.Root) { return }
    $resolvedRoot = [System.IO.Path]::GetFullPath([string]$Snapshot.Root)
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $tempPrefix = $tempRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    $leaf = [System.IO.Path]::GetFileName($resolvedRoot)
    if (-not $resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        $leaf -notmatch '^sura_test_engine_[0-9a-f]{32}$') {
        throw "refusing to remove an invalid Sura engine snapshot path: $resolvedRoot"
    }
    if (Test-Path -LiteralPath $resolvedRoot) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction Stop
    }
}

function ConvertTo-SuraProcessArgument {
    param([AllowEmptyString()][string]$Argument)

    if ($null -eq $Argument -or $Argument.Length -eq 0) { return '""' }
    if ($Argument -notmatch '[\s"]') { return $Argument }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($ch in $Argument.ToCharArray()) {
        if ($ch -eq '\') {
            $backslashes++
            continue
        }
        if ($ch -eq '"') {
            [void]$builder.Append(('\' * ($backslashes * 2 + 1)))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($ch)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Stop-SuraProcessTree {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) { return }
    try { if ($Process.HasExited) { return } } catch { return }

    if ($env:OS -eq "Windows_NT") {
        $taskkill = Get-Command taskkill.exe -ErrorAction SilentlyContinue
        if ($taskkill) {
            try { & $taskkill.Source /PID $Process.Id /T /F 2>&1 | Out-Null } catch {}
        }
    } else {
        try {
            $rows = @(& ps -A -o pid= -o ppid= 2>$null)
            $childrenByParent = @{}
            foreach ($row in $rows) {
                if ([string]$row -notmatch '^\s*(\d+)\s+(\d+)\s*$') { continue }
                $pid = [int]$Matches[1]
                $ppid = [int]$Matches[2]
                if (-not $childrenByParent.ContainsKey($ppid)) {
                    $childrenByParent[$ppid] = New-Object System.Collections.Generic.List[int]
                }
                $childrenByParent[$ppid].Add($pid)
            }
            $pending = New-Object System.Collections.Generic.Stack[int]
            $ordered = New-Object System.Collections.Generic.List[int]
            $pending.Push($Process.Id)
            while ($pending.Count -gt 0) {
                $parent = $pending.Pop()
                if (-not $childrenByParent.ContainsKey($parent)) { continue }
                foreach ($child in $childrenByParent[$parent]) {
                    $ordered.Add($child)
                    $pending.Push($child)
                }
            }
            for ($i = $ordered.Count - 1; $i -ge 0; $i--) {
                Stop-Process -Id $ordered[$i] -Force -ErrorAction SilentlyContinue
            }
        } catch {}
    }

    try {
        if (-not $Process.HasExited) { $Process.Kill() }
    } catch {}
    try { [void]$Process.WaitForExit(5000) } catch {}
}

function Invoke-SuraTestProcess {
    param(
        [Parameter(Mandatory=$true)][string]$EnginePath,
        [string[]]$Arguments = @(),
        [ValidateRange(1, 86400)][int]$TimeoutSeconds = 120,
        [string]$WorkingDirectory = ""
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $EnginePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $startInfo.WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
    }
    $argumentList = $startInfo.PSObject.Properties["ArgumentList"]
    if ($null -ne $argumentList) {
        foreach ($argument in @($Arguments)) {
            [void]$startInfo.ArgumentList.Add([string]$argument)
        }
    } else {
        $startInfo.Arguments = (@($Arguments) | ForEach-Object {
            ConvertTo-SuraProcessArgument ([string]$_)
        }) -join " "
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $peakWorkingSetBytes = [int64]0
    $timedOut = $false
    while ($true) {
        $remainingMs = ($TimeoutSeconds * 1000) - $stopwatch.ElapsedMilliseconds
        if ($remainingMs -le 0) {
            $timedOut = $true
            break
        }
        $waitMs = [int][Math]::Min(50, $remainingMs)
        if ($process.WaitForExit($waitMs)) { break }
        try {
            $process.Refresh()
            $sample = [int64]($process.PeakWorkingSet64)
            if ($sample -gt $peakWorkingSetBytes) { $peakWorkingSetBytes = $sample }
        } catch {}
    }
    if ($timedOut) {
        Stop-SuraProcessTree $process
    } else {
        # The parameterless wait flushes redirected asynchronous streams.
        $process.WaitForExit()
    }
    $stopwatch.Stop()

    $stdout = try { [string]$stdoutTask.Result } catch { "" }
    $stderr = try { [string]$stderrTask.Result } catch { "" }
    $parts = @()
    if (-not [string]::IsNullOrEmpty($stdout)) { $parts += $stdout.TrimEnd("`r", "`n") }
    if (-not [string]::IsNullOrEmpty($stderr)) { $parts += $stderr.TrimEnd("`r", "`n") }
    $output = $parts -join "`n"
    $exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
    $processId = [int]$process.Id
    try {
        $sample = [int64]($process.PeakWorkingSet64)
        if ($sample -gt $peakWorkingSetBytes) { $peakWorkingSetBytes = $sample }
    } catch {}
    $process.Dispose()

    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        TimedOut = [bool]$timedOut
        TimeoutSeconds = [int]$TimeoutSeconds
        DurationMs = [int64]$stopwatch.ElapsedMilliseconds
        ProcessId = $processId
        PeakWorkingSetBytes = $peakWorkingSetBytes
        Output = $output
    }
}
