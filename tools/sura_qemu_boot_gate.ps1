param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/qemu_boot_gate.sura"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [string]$DataDisk = "",
    [switch]$PersistDataDisk,
    [switch]$EnableNetwork,
    [int]$TimeoutSeconds = 30,
    [string]$ExpectedEfiText = "Sura QEMU boot gate",
    [string]$ExpectedMarker = "SURA_EXIT_BOOT_SERVICES_OK",
    [int]$ExpectedExitCode = 33,
    [string[]]$SerialInputLines = @(),
    [int]$SerialInputDelayMilliseconds = 5000,
    [int]$SerialInputIntervalMilliseconds = 100,
    [string[]]$AdditionalExpectedSerialMarkers = @(),
    [string[]]$AdditionalQemuArguments = @(),
    [ValidateRange(0, 1024)]
    [int]$TcgTranslationCacheMiB = 0,
    [string]$QmpSendKey = "",
    [string]$QmpInputDevice = "",
    [int]$QmpInputDelayMilliseconds = 2000,
    [switch]$QmpKeyDownOnly,
    [int]$QmpMouseDeltaX = 0,
    [int]$QmpMouseDeltaY = 0,
    [string]$QmpScreendumpPath = "",
    [switch]$DisablePs2,
    [switch]$HeadlessVnc,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_qemu_boot_" + [guid]::NewGuid().ToString("N"))

function Resolve-Qemu {
    param([string]$Requested)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        if (-not (Test-Path -LiteralPath $Requested -PathType Leaf)) {
            throw "QEMU was not found: $Requested"
        }
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $command = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $standard = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (Test-Path -LiteralPath $standard -PathType Leaf) { return $standard }
    throw "QEMU x86-64 was not found. Install QEMU or pass -Qemu <path>."
}

function Resolve-Firmware {
    param([string]$Requested, [string]$QemuPath)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        if (-not (Test-Path -LiteralPath $Requested -PathType Leaf)) {
            throw "UEFI firmware was not found: $Requested"
        }
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $qemuRoot = Split-Path -Parent $QemuPath
    foreach ($candidate in @(
        (Join-Path $qemuRoot "share/edk2-x86_64-code.fd"),
        (Join-Path $qemuRoot "share/OVMF_CODE.fd"),
        "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
        "C:\Program Files\qemu\share\OVMF_CODE.fd"
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "OVMF/EDK2 x86-64 firmware was not found. Pass -Firmware <path>."
}

function ConvertTo-NativeArgument {
    param([string]$Value)
    if ($Value.IndexOf([char]0) -ge 0) {
        throw "QEMU argument contains a null character"
    }
    if ($Value -notmatch '[\s"]') { return $Value }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
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

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback,
        0
    )
    try {
        $listener.Start()
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    }
    finally {
        $listener.Stop()
    }
}

function Get-FreeVncDisplay {
    foreach ($display in 40..99) {
        $listener = [System.Net.Sockets.TcpListener]::new(
            [System.Net.IPAddress]::Loopback,
            5900 + $display
        )
        try {
            $listener.Start()
            return $display
        }
        catch {
        }
        finally {
            $listener.Stop()
        }
    }
    throw "No free local VNC display was found"
}

function Connect-Qmp {
    param([int]$Port, [datetime]$Deadline)
    while ([datetime]::UtcNow -lt $Deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $client.Connect("127.0.0.1", $Port)
            return $client
        }
        catch {
            $client.Dispose()
            Start-Sleep -Milliseconds 50
        }
    }
    throw "Could not connect to QEMU QMP on port $Port"
}

function Invoke-Qmp {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [hashtable]$Command
    )
    $Writer.WriteLine(($Command | ConvertTo-Json -Compress -Depth 8))
    $Writer.Flush()
    while ($true) {
        $line = $Reader.ReadLine()
        if ($null -eq $line) { throw "QEMU QMP connection closed" }
        $response = $line | ConvertFrom-Json
        if ($null -ne $response.error) {
            throw "QEMU QMP error: $($response.error | ConvertTo-Json -Compress)"
        }
        if ($response.PSObject.Properties.Name -contains "return") {
            return $response.return
        }
    }
}

try {
    if ($PersistDataDisk -and [string]::IsNullOrWhiteSpace($DataDisk)) {
        throw "PersistDataDisk requires DataDisk"
    }
    if ($PersistDataDisk -and $CompileOnly) {
        throw "PersistDataDisk cannot be used with CompileOnly"
    }
    if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 300) {
        throw "TimeoutSeconds must be 1..300"
    }
    if ($SerialInputDelayMilliseconds -lt 0 -or
        $SerialInputDelayMilliseconds -gt $TimeoutSeconds * 1000) {
        throw "SerialInputDelayMilliseconds must be 0..TimeoutSeconds*1000"
    }
    if ($SerialInputIntervalMilliseconds -lt 0 -or
        $SerialInputIntervalMilliseconds -gt $TimeoutSeconds * 1000) {
        throw "SerialInputIntervalMilliseconds must be 0..TimeoutSeconds*1000"
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedEfiText) -or
        [string]::IsNullOrWhiteSpace($ExpectedMarker)) {
        throw "ExpectedEfiText and ExpectedMarker must not be empty"
    }
    if ($AdditionalQemuArguments.Count -gt 128) {
        throw "AdditionalQemuArguments supports at most 128 values"
    }
    if ($QmpInputDelayMilliseconds -lt 0 -or
        $QmpInputDelayMilliseconds -gt $TimeoutSeconds * 1000) {
        throw "QmpInputDelayMilliseconds must be 0..TimeoutSeconds*1000"
    }
    if ($QmpMouseDeltaX -lt -32768 -or $QmpMouseDeltaX -gt 32767 -or
        $QmpMouseDeltaY -lt -32768 -or $QmpMouseDeltaY -gt 32767) {
        throw "QmpMouseDeltaX and QmpMouseDeltaY must be -32768..32767"
    }
    if (-not [string]::IsNullOrWhiteSpace($QmpScreendumpPath)) {
        if ($QmpScreendumpPath.IndexOf([char]0) -ge 0 -or
            $QmpScreendumpPath.Contains("`r") -or
            $QmpScreendumpPath.Contains("`n")) {
            throw "QmpScreendumpPath contains an invalid value"
        }
        $screendumpParent = Split-Path -Parent ([System.IO.Path]::GetFullPath($QmpScreendumpPath))
        if ([string]::IsNullOrWhiteSpace($screendumpParent) -or
            -not (Test-Path -LiteralPath $screendumpParent -PathType Container)) {
            throw "QmpScreendumpPath parent directory was not found"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($QmpSendKey) -and
        ($QmpSendKey.IndexOf([char]0) -ge 0 -or
         $QmpSendKey.Contains("`r") -or
         $QmpSendKey.Contains("`n") -or
         $QmpSendKey.Length -gt 32)) {
        throw "QmpSendKey contains an invalid value"
    }
    if (-not [string]::IsNullOrWhiteSpace($QmpInputDevice) -and
        ($QmpInputDevice.IndexOf([char]0) -ge 0 -or
         $QmpInputDevice.Contains("`r") -or
         $QmpInputDevice.Contains("`n") -or
         $QmpInputDevice.Length -gt 64)) {
        throw "QmpInputDevice contains an invalid value"
    }
    foreach ($argument in $AdditionalQemuArguments) {
        if ($null -eq $argument -or $argument.IndexOf([char]0) -ge 0 -or
            $argument.Contains("`r") -or $argument.Contains("`n")) {
            throw "AdditionalQemuArguments contains an invalid value"
        }
    }
    if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
        throw "Sura engine was not found: $Engine"
    }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "QEMU boot source was not found: $Source"
    }
    if (-not [string]::IsNullOrWhiteSpace($DataDisk) -and
        -not (Test-Path -LiteralPath $DataDisk -PathType Leaf)) {
        throw "QEMU data disk was not found: $DataDisk"
    }

    New-Item -ItemType Directory -Path $temp | Out-Null
    $efi = Join-Path $temp "BOOTX64.EFI"
    $disk = Join-Path $temp "sura-qemu.img"
    $compileOutput = & $Engine --target uefi-x86_64 --out $efi --disk-image $disk $Source 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "QEMU boot image compile failed:`n$($compileOutput -join "`n")"
    }
    if (-not (Test-Path -LiteralPath $efi -PathType Leaf) -or
        -not (Test-Path -LiteralPath $disk -PathType Leaf)) {
        throw "QEMU boot image compile did not produce both EFI and disk files"
    }

    $efiBytes = [System.IO.File]::ReadAllBytes($efi)
    $efiUtf16 = [System.Text.Encoding]::Unicode.GetString($efiBytes)
    $efiAscii = [System.Text.Encoding]::ASCII.GetString($efiBytes)
    if ($efiBytes.Length -lt 70000 -or
        $efiBytes[0] -ne 0x4d -or $efiBytes[1] -ne 0x5a -or
        -not $efiUtf16.Contains($ExpectedEfiText) -or
        -not $efiAscii.Contains($ExpectedMarker)) {
        throw "QEMU boot EFI image is missing its required boot markers"
    }

    if ($CompileOnly) {
        "sura_qemu_boot_gate: COMPILE PASS (efi=$($efiBytes.Length), disk=$((Get-Item -LiteralPath $disk).Length) bytes)"
        return
    }

    $qemuPath = Resolve-Qemu $Qemu
    $firmwarePath = Resolve-Firmware $Firmware $qemuPath
    $temporaryDataDisk = ""
    if (-not [string]::IsNullOrWhiteSpace($DataDisk)) {
        $temporaryDataDisk = Join-Path $temp "sura-data.img"
        Copy-Item -LiteralPath $DataDisk -Destination $temporaryDataDisk -Force
    }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $process.StartInfo.FileName = $qemuPath
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardInput = $SerialInputLines.Count -gt 0
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $machineOptions = "q35,accel=tcg"
    if ($TcgTranslationCacheMiB -gt 0) {
        $machineOptions = "q35"
    }
    if ($DisablePs2) {
        $machineOptions += ",i8042=off"
    }
    $displayOptions = "none"
    if ($HeadlessVnc) {
        $vncDisplay = Get-FreeVncDisplay
        $displayOptions = "vnc=127.0.0.1:$vncDisplay"
    }
    $qemuArguments = @(
        "-machine", $machineOptions,
        "-m", "256M",
        "-display", $displayOptions,
        "-monitor", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-drive", "if=pflash,format=raw,readonly=on,file=$firmwarePath",
        "-drive", "file=$disk,format=raw,if=ide,index=0",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "c"
    )
    if ($TcgTranslationCacheMiB -gt 0) {
        $qemuArguments += @(
            "-accel", "tcg,tb-size=$TcgTranslationCacheMiB"
        )
    }
    $qmpPort = 0
    $useQmp = -not [string]::IsNullOrWhiteSpace($QmpSendKey) -or
        $QmpMouseDeltaX -ne 0 -or $QmpMouseDeltaY -ne 0 -or
        -not [string]::IsNullOrWhiteSpace($QmpScreendumpPath)
    if ($useQmp) {
        $qmpPort = Get-FreeTcpPort
        $qemuArguments += @(
            "-qmp", "tcp:127.0.0.1:$qmpPort,server=on,wait=off"
        )
    }
    if (-not [string]::IsNullOrWhiteSpace($temporaryDataDisk)) {
        $qemuArguments += @(
            "-drive", "file=$temporaryDataDisk,format=raw,if=ide,index=1"
        )
    }
    if ($EnableNetwork) {
        $qemuArguments += @(
            "-netdev", "user,id=suranet",
            "-device", "virtio-net-pci,netdev=suranet,disable-modern=on,mac=52:54:00:12:34:56"
        )
    }
    if ($AdditionalQemuArguments.Count -gt 0) {
        $qemuArguments += $AdditionalQemuArguments
    }
    if ($process.StartInfo.PSObject.Properties.Name -contains "ArgumentList") {
        foreach ($argument in $qemuArguments) {
            $process.StartInfo.ArgumentList.Add($argument)
        }
    }
    else {
        # Windows PowerShell 5.1 uses .NET Framework and has no ArgumentList.
        $process.StartInfo.Arguments =
            (($qemuArguments | ForEach-Object {
                ConvertTo-NativeArgument ([string]$_)
            }) -join " ")
    }

    if (-not $process.Start()) { throw "QEMU did not start" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if ($useQmp) {
        Start-Sleep -Milliseconds $QmpInputDelayMilliseconds
        $qmpClient = Connect-Qmp $qmpPort ([datetime]::UtcNow.AddSeconds(5))
        try {
            $qmpStream = $qmpClient.GetStream()
            $qmpStream.ReadTimeout = 5000
            $qmpReader = [System.IO.StreamReader]::new(
                $qmpStream,
                [System.Text.Encoding]::UTF8,
                $false,
                1024,
                $true
            )
            $qmpWriter = [System.IO.StreamWriter]::new(
                $qmpStream,
                [System.Text.UTF8Encoding]::new($false),
                1024,
                $true
            )
            $qmpWriter.NewLine = "`n"
            $greeting = $qmpReader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($greeting) -or
                -not $greeting.Contains('"QMP"')) {
                throw "QEMU did not return a QMP greeting"
            }
            [void](Invoke-Qmp $qmpReader $qmpWriter @{ execute = "qmp_capabilities" })
            try {
                if (-not [string]::IsNullOrWhiteSpace($QmpSendKey) -and
                    $QmpKeyDownOnly) {
                    $inputArguments = @{
                        events = @(
                            @{
                                type = "key"
                                data = @{
                                    down = $true
                                    key = @{
                                        type = "qcode"
                                        data = $QmpSendKey
                                    }
                                }
                            }
                        )
                    }
                    if (-not [string]::IsNullOrWhiteSpace($QmpInputDevice)) {
                        $inputArguments.device = $QmpInputDevice
                    }
                    [void](Invoke-Qmp $qmpReader $qmpWriter @{
                        execute = "input-send-event"
                        arguments = $inputArguments
                    })
                }
                elseif (-not [string]::IsNullOrWhiteSpace($QmpSendKey) -and
                        [string]::IsNullOrWhiteSpace($QmpInputDevice)) {
                    [void](Invoke-Qmp $qmpReader $qmpWriter @{
                        execute = "human-monitor-command"
                        arguments = @{ "command-line" = "sendkey $QmpSendKey" }
                    })
                }
                elseif (-not [string]::IsNullOrWhiteSpace($QmpSendKey)) {
                    [void](Invoke-Qmp $qmpReader $qmpWriter @{
                        execute = "input-send-event"
                        arguments = @{
                            device = $QmpInputDevice
                            events = @(
                                @{
                                    type = "key"
                                    data = @{
                                        down = $true
                                        key = @{
                                            type = "qcode"
                                            data = $QmpSendKey
                                        }
                                    }
                                }
                            )
                        }
                    })
                }
                if ($QmpMouseDeltaX -ne 0 -or $QmpMouseDeltaY -ne 0) {
                    [void](Invoke-Qmp $qmpReader $qmpWriter @{
                        execute = "human-monitor-command"
                        arguments = @{
                            "command-line" = "mouse_move $QmpMouseDeltaX $QmpMouseDeltaY"
                        }
                    })
                }
                if (-not [string]::IsNullOrWhiteSpace($QmpScreendumpPath)) {
                    [void](Invoke-Qmp $qmpReader $qmpWriter @{
                        execute = "screendump"
                        arguments = @{
                            filename = [System.IO.Path]::GetFullPath($QmpScreendumpPath)
                            format = "ppm"
                        }
                    })
                }
            }
            catch {
                # A successful injected event may let a short boot gate exit
                # before QMP can return its empty response.
                if (-not $process.HasExited) { throw }
            }
        }
        finally {
            if ($null -ne $qmpWriter) { $qmpWriter.Dispose() }
            if ($null -ne $qmpReader) { $qmpReader.Dispose() }
            $qmpClient.Dispose()
        }
    }
    if ($SerialInputLines.Count -gt 0) {
        Start-Sleep -Milliseconds $SerialInputDelayMilliseconds
        foreach ($line in $SerialInputLines) {
            $process.StandardInput.WriteLine($line)
            $process.StandardInput.Flush()
            Start-Sleep -Milliseconds $SerialInputIntervalMilliseconds
        }
        $process.StandardInput.Close()
    }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        $process.WaitForExit()
        $timedOutSerialOutput = $stdoutTask.GetAwaiter().GetResult()
        $timedOutDiagnosticOutput = $stderrTask.GetAwaiter().GetResult()
        throw "QEMU boot timed out after $TimeoutSeconds seconds:`n$timedOutSerialOutput`n$timedOutDiagnosticOutput"
    }
    $serialOutput = $stdoutTask.GetAwaiter().GetResult()
    $diagnosticOutput = $stderrTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne $ExpectedExitCode -or
        -not $serialOutput.Contains($ExpectedMarker)) {
        throw "QEMU boot failed (exit=$($process.ExitCode)):`n$serialOutput`n$diagnosticOutput"
    }
    foreach ($marker in $AdditionalExpectedSerialMarkers) {
        if ([string]::IsNullOrWhiteSpace($marker)) {
            throw "AdditionalExpectedSerialMarkers must not contain empty values"
        }
        if (-not $serialOutput.Contains($marker)) {
            throw "QEMU boot output is missing marker '$marker':`n$serialOutput`n$diagnosticOutput"
        }
    }
    if ($PersistDataDisk) {
        Copy-Item -LiteralPath $temporaryDataDisk -Destination $DataDisk -Force
    }

    "sura_qemu_boot_gate: PASS (exit=$($process.ExitCode), marker=$ExpectedMarker)"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolved = [System.IO.Path]::GetFullPath($temp)
        $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_qemu_boot_")) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
