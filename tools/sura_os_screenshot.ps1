param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [string]$Output = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-desktop.ppm"),
    [int]$TimeoutSeconds = 30,
    [switch]$SkipInputVerification
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$disk = Join-Path $root "build/os/SuraOS.img"

function Resolve-SuraOsQemu {
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

function Resolve-SuraOsFirmware {
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
        (Join-Path $qemuRoot "share/OVMF_CODE.fd")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "OVMF/EDK2 x86-64 firmware was not found. Pass -Firmware <path>."
}

function ConvertTo-SuraOsNativeArgument {
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

function Get-SuraOsFreeTcpPort {
    $listener = New-Object System.Net.Sockets.TcpListener(
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

function Connect-SuraOsTcp {
    param([int]$Port, [DateTime]$Deadline)
    while ([DateTime]::UtcNow -lt $Deadline) {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect("127.0.0.1", $Port)
            return $client
        }
        catch {
            $client.Dispose()
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Could not connect to QEMU TCP port $Port"
}

function Invoke-SuraOsQmp {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [hashtable]$Command
    )
    $Writer.WriteLine(($Command | ConvertTo-Json -Compress -Depth 6))
    $Writer.Flush()
    while ($true) {
        $line = $Reader.ReadLine()
        if ($null -eq $line) { throw "QEMU QMP connection closed" }
        $response = $line | ConvertFrom-Json
        if ($null -ne $response.error) {
            throw "QEMU QMP error: $($response.error | ConvertTo-Json -Compress)"
        }
        if ($response.PSObject.Properties.Name -contains "return") {
            return $response
        }
    }
}

$qemuProcess = $null
$serialClient = $null
$qmpClient = $null
$serialStream = $null
$qmpReader = $null
$qmpWriter = $null
$temporaryDisk = $null
$temporaryCapture = $null

try {
    & (Join-Path $PSScriptRoot "sura_os_vm.ps1") `
        -Engine $Engine `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -CompileOnly
    if ($LASTEXITCODE -ne 0) { throw "Sura OS compile-only verification failed" }

    $qemuPath = Resolve-SuraOsQemu $Qemu
    $firmwarePath = Resolve-SuraOsFirmware $Firmware $qemuPath
    $token = [guid]::NewGuid().ToString("N")
    $temporaryDisk = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$token.img"
    $temporaryCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$token.ppm"
    Copy-Item -LiteralPath $disk -Destination $temporaryDisk -Force

    $serialPort = Get-SuraOsFreeTcpPort
    $qmpPort = Get-SuraOsFreeTcpPort
    while ($qmpPort -eq $serialPort) { $qmpPort = Get-SuraOsFreeTcpPort }
    $qemuArguments = @(
        "-machine", "q35,accel=tcg",
        "-m", "256M",
        "-display", "none",
        "-monitor", "none",
        "-qmp", "tcp:127.0.0.1:$qmpPort,server=on,wait=off",
        "-serial", "tcp:127.0.0.1:$serialPort,server=on,wait=off",
        "-no-reboot",
        "-drive", "if=pflash,format=raw,readonly=on,file=$firmwarePath",
        "-drive", "file=$temporaryDisk,format=raw,if=ide",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "c"
    )

    $qemuProcess = New-Object System.Diagnostics.Process
    $qemuProcess.StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $qemuProcess.StartInfo.FileName = $qemuPath
    $qemuProcess.StartInfo.UseShellExecute = $false
    $qemuProcess.StartInfo.CreateNoWindow = $true
    $qemuProcess.StartInfo.RedirectStandardOutput = $true
    $qemuProcess.StartInfo.RedirectStandardError = $true
    if ($qemuProcess.StartInfo.PSObject.Properties.Name -contains "ArgumentList") {
        foreach ($argument in $qemuArguments) {
            $qemuProcess.StartInfo.ArgumentList.Add($argument)
        }
    }
    else {
        $qemuProcess.StartInfo.Arguments =
            (($qemuArguments | ForEach-Object {
                ConvertTo-SuraOsNativeArgument ([string]$_)
            }) -join " ")
    }
    if (-not $qemuProcess.Start()) { throw "QEMU did not start" }
    $qemuStdoutTask = $qemuProcess.StandardOutput.ReadToEndAsync()
    $qemuStderrTask = $qemuProcess.StandardError.ReadToEndAsync()

    $connectDeadline = [DateTime]::UtcNow.AddSeconds(8)
    $serialClient = Connect-SuraOsTcp $serialPort $connectDeadline
    $qmpClient = Connect-SuraOsTcp $qmpPort $connectDeadline
    $serialStream = $serialClient.GetStream()
    $qmpStream = $qmpClient.GetStream()
    $qmpStream.ReadTimeout = 5000
    $qmpReader = New-Object System.IO.StreamReader(
        $qmpStream,
        (New-Object System.Text.UTF8Encoding($false)),
        $false,
        1024,
        $true
    )
    $qmpWriter = New-Object System.IO.StreamWriter(
        $qmpStream,
        (New-Object System.Text.UTF8Encoding($false)),
        1024,
        $true
    )
    $qmpWriter.NewLine = "`n"
    $greeting = $qmpReader.ReadLine()
    if ([string]::IsNullOrWhiteSpace($greeting) -or -not $greeting.Contains('"QMP"')) {
        throw "QEMU did not return a QMP greeting"
    }
    [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{ execute = "qmp_capabilities" })

    $serialText = New-Object System.Text.StringBuilder
    $serialBuffer = New-Object byte[] 4096
    $bootDeadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $bootDeadline -and
           (-not $serialText.ToString().Contains("SURA_OS_DESKTOP_OK") -or
            (-not $SkipInputVerification -and
             (-not $serialText.ToString().Contains("SURA_OS_PS2_READY") -or
              -not $serialText.ToString().Contains("Sura OS shell ready"))))) {
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        if ($qemuProcess.HasExited) { break }
        Start-Sleep -Milliseconds 50
    }
    if (-not $serialText.ToString().Contains("SURA_OS_DESKTOP_OK")) {
        throw "Sura OS desktop marker was not observed before capture"
    }

    if (-not $SkipInputVerification) {
        if (-not $serialText.ToString().Contains("SURA_OS_PS2_READY")) {
            throw "Sura OS PS/2 controller did not become ready"
        }
        foreach ($key in @("shift", "s", "t", "a", "t", "u", "z", "backspace", "s", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 80
        }
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "mouse_move 24 12" }
        })
        Start-Sleep -Milliseconds 120
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "input-send-event"
            arguments = @{
                events = @(
                    @{ type = "btn"; data = @{ button = "left"; down = $true } }
                )
            }
        })
        Start-Sleep -Milliseconds 120
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "input-send-event"
            arguments = @{
                events = @(
                    @{ type = "btn"; data = @{ button = "left"; down = $false } }
                )
            }
        })

        $inputDeadline = [DateTime]::UtcNow.AddSeconds(8)
        while ([DateTime]::UtcNow -lt $inputDeadline -and
               (-not $serialText.ToString().Contains("SURA_OS_KEYBOARD_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SHIFT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_MOUSE_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_MOUSE_CLICK_OK") -or
                -not $serialText.ToString().Contains("kernel: ready"))) {
            while ($serialStream.DataAvailable) {
                $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                if ($read -le 0) { break }
                [void]$serialText.Append(
                    [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                )
            }
            if ($qemuProcess.HasExited) { break }
            Start-Sleep -Milliseconds 50
        }
        foreach ($marker in @(
            "SURA_OS_KEYBOARD_OK",
            "SURA_OS_SHIFT_OK",
            "SURA_OS_MOUSE_OK",
            "SURA_OS_MOUSE_CLICK_OK",
            "kernel: ready"
        )) {
            if (-not $serialText.ToString().Contains($marker)) {
                throw "Sura OS input verification did not observe: $marker"
            }
        }
    }

    $qemuCapturePath = $temporaryCapture.Replace('\', '/')
    [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
        execute = "screendump"
        arguments = @{ filename = $qemuCapturePath }
    })
    if (-not (Test-Path -LiteralPath $temporaryCapture -PathType Leaf)) {
        throw "QEMU did not create the desktop screenshot"
    }

    $outputDirectory = Split-Path -Parent $Output
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $temporaryCapture -Destination $Output -Force

    $shutdownBytes = [System.Text.Encoding]::ASCII.GetBytes("shutdown`n")
    $serialStream.Write($shutdownBytes, 0, $shutdownBytes.Length)
    $serialStream.Flush()
    if (-not $qemuProcess.WaitForExit(10000)) {
        throw "QEMU did not shut down after the screenshot"
    }
    if ($qemuProcess.ExitCode -ne 33) {
        $diagnostics = $qemuStderrTask.GetAwaiter().GetResult()
        throw "QEMU closed with unexpected exit code $($qemuProcess.ExitCode)`n$diagnostics"
    }

    $capture = Get-Item -LiteralPath $Output
    $inputStatus = "verified"
    if ($SkipInputVerification) { $inputStatus = "skipped" }
    "sura_os_screenshot: PASS ($($capture.FullName), $($capture.Length) bytes, input=$inputStatus)"
}
catch {
    if ($null -ne $qemuProcess -and $qemuProcess.HasExited) {
        try {
            $diagnostics = $qemuProcess.StandardError.ReadToEnd()
            if (-not [string]::IsNullOrWhiteSpace($diagnostics)) {
                Write-Host $diagnostics
            }
        }
        catch {}
    }
    Write-Error $_
    exit 1
}
finally {
    if ($null -ne $qmpWriter) { $qmpWriter.Dispose() }
    if ($null -ne $qmpReader) { $qmpReader.Dispose() }
    if ($null -ne $serialStream) { $serialStream.Dispose() }
    if ($null -ne $qmpClient) { $qmpClient.Dispose() }
    if ($null -ne $serialClient) { $serialClient.Dispose() }
    if ($null -ne $qemuProcess) {
        if (-not $qemuProcess.HasExited) {
            $qemuProcess.Kill()
            $qemuProcess.WaitForExit()
        }
        $qemuProcess.Dispose()
    }
    foreach ($temporaryPath in @($temporaryDisk, $temporaryCapture)) {
        if (-not [string]::IsNullOrWhiteSpace($temporaryPath) -and
            (Test-Path -LiteralPath $temporaryPath -PathType Leaf)) {
            $resolvedTemporaryPath = [System.IO.Path]::GetFullPath($temporaryPath)
            $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
            if ($resolvedTemporaryPath.StartsWith(
                    $resolvedTempRoot,
                    [System.StringComparison]::OrdinalIgnoreCase
                ) -and
                (Split-Path -Leaf $resolvedTemporaryPath).StartsWith("sura_os_capture_")) {
                Remove-Item -LiteralPath $resolvedTemporaryPath -Force
            }
        }
    }
}
