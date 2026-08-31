param(
    [string]$Qemu = "",
    [string]$Firmware = "",
    [ValidateRange(30, 300)]
    [int]$TimeoutSeconds = 150,
    [string]$ScreenshotPath = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $root "build/os"
$disk = Join-Path $outputDirectory "SuraOS.img"
$dataDisk = Join-Path $outputDirectory "SuraData.img"
$serialLog = Join-Path $outputDirectory "SuraOS-Doom.serial.log"
if ([string]::IsNullOrWhiteSpace($ScreenshotPath)) {
    $ScreenshotPath = Join-Path $outputDirectory "SuraOS-Doom.ppm"
}

function Resolve-DesktopDoomQemu {
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

function Resolve-DesktopDoomFirmware {
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

function ConvertTo-DesktopDoomNativeArgument {
    param([string]$Value)
    if ($Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new()
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

function Get-DesktopDoomFreeTcpPort {
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

function Connect-DesktopDoomQmp {
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

function Invoke-DesktopDoomQmp {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [hashtable]$Command
    )
    $Writer.WriteLine(($Command | ConvertTo-Json -Compress -Depth 10))
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

function Read-DesktopDoomSerial {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite
    )
    try {
        $reader = [System.IO.StreamReader]::new(
            $stream,
            [System.Text.Encoding]::ASCII,
            $false,
            4096,
            $true
        )
        try { return $reader.ReadToEnd() }
        finally { $reader.Dispose() }
    }
    finally {
        $stream.Dispose()
    }
}

function Wait-DesktopDoomMarker {
    param(
        [string]$Path,
        [string]$Marker,
        [datetime]$Deadline,
        [System.Diagnostics.Process]$Process
    )
    while ([datetime]::UtcNow -lt $Deadline) {
        $serial = Read-DesktopDoomSerial $Path
        if ($serial.Contains($Marker)) { return $serial }
        if ($Process.HasExited) {
            throw "QEMU exited before '$Marker' appeared (exit=$($Process.ExitCode))`n$serial"
        }
        Start-Sleep -Milliseconds 100
    }
    $serial = Read-DesktopDoomSerial $Path
    throw "Timed out waiting for '$Marker'`n$serial"
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "sura_os_vm.ps1") -CompileOnly
    if ($LASTEXITCODE -ne 0) { throw "Sura OS Doom desktop build failed" }
}
if (-not (Test-Path -LiteralPath $disk -PathType Leaf)) {
    throw "Patched Sura OS disk was not found: $disk"
}
if (-not (Test-Path -LiteralPath $dataDisk -PathType Leaf)) {
    throw "Sura OS data disk was not found: $dataDisk"
}

$qemuPath = Resolve-DesktopDoomQemu $Qemu
$firmwarePath = Resolve-DesktopDoomFirmware $Firmware $qemuPath
$runtimeDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_os_doom_desktop_" + [guid]::NewGuid().ToString("N"))
$runtimeDisk = Join-Path $runtimeDirectory "SuraOS.img"
$runtimeDataDisk = Join-Path $runtimeDirectory "SuraData.img"
$runtimeSerial = Join-Path $runtimeDirectory "serial.log"
$runtimeScreenshot = Join-Path $runtimeDirectory "doom.ppm"
$process = $null
$qmpClient = $null
$qmpReader = $null
$qmpWriter = $null

try {
    New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null
    Copy-Item -LiteralPath $disk -Destination $runtimeDisk -Force
    Copy-Item -LiteralPath $dataDisk -Destination $runtimeDataDisk -Force
    $qmpPort = Get-DesktopDoomFreeTcpPort
    $qemuArguments = @(
        "-machine", "q35,accel=tcg",
        "-cpu", "max",
        "-m", "256M",
        "-display", "none",
        "-monitor", "none",
        "-serial", "file:$runtimeSerial",
        "-qmp", "tcp:127.0.0.1:$qmpPort,server=on,wait=off",
        "-no-reboot",
        "-drive", "if=pflash,format=raw,readonly=on,file=$firmwarePath",
        "-drive", "file=$runtimeDisk,format=raw,if=ide,index=0",
        "-drive", "file=$runtimeDataDisk,format=raw,if=ide,index=1",
        "-netdev", "user,id=suranet",
        "-device", "virtio-net-pci,netdev=suranet,disable-modern=on,mac=52:54:00:12:34:56",
        "-device", "qemu-xhci,id=sura-xhci",
        "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
        "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0",
        "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=1280,yres=800",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "c"
    )

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $process.StartInfo.FileName = $qemuPath
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    if ($process.StartInfo.PSObject.Properties.Name -contains "ArgumentList") {
        foreach ($argument in $qemuArguments) {
            $process.StartInfo.ArgumentList.Add($argument)
        }
    }
    else {
        $process.StartInfo.Arguments =
            (($qemuArguments | ForEach-Object {
                ConvertTo-DesktopDoomNativeArgument ([string]$_)
            }) -join " ")
    }
    if (-not $process.Start()) { throw "QEMU did not start" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_OS_DOOM_EMBEDDED_READY" $deadline $process)
    [void](Wait-DesktopDoomMarker $runtimeSerial "Sura OS shell ready" $deadline $process)
    Write-Host "Desktop ready; clicking the Doom icon..."

    $qmpClient = Connect-DesktopDoomQmp $qmpPort ([datetime]::UtcNow.AddSeconds(5))
    $qmpStream = $qmpClient.GetStream()
    $qmpStream.ReadTimeout = 5000
    $qmpReader = [System.IO.StreamReader]::new($qmpStream, [System.Text.Encoding]::UTF8, $false, 1024, $true)
    $qmpWriter = [System.IO.StreamWriter]::new($qmpStream, [System.Text.UTF8Encoding]::new($false), 1024, $true)
    $qmpWriter.NewLine = "`n"
    $greeting = $qmpReader.ReadLine()
    if ([string]::IsNullOrWhiteSpace($greeting) -or -not $greeting.Contains('"QMP"')) {
        throw "QEMU did not return a QMP greeting"
    }
    [void](Invoke-DesktopDoomQmp $qmpReader $qmpWriter @{ execute = "qmp_capabilities" })

    foreach ($move in @(
        @(-120, 52),
        @(-120, 52),
        @(-120, 52),
        @(-120, 53),
        @(-92, 0)
    )) {
        [void](Invoke-DesktopDoomQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{
                "command-line" = "mouse_move $($move[0]) $($move[1])"
            }
        })
        Start-Sleep -Milliseconds 120
    }
    foreach ($buttons in @(1, 0)) {
        [void](Invoke-DesktopDoomQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{
                "command-line" = "mouse_button $buttons"
            }
        })
        Start-Sleep -Milliseconds 150
    }

    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_OS_DOOM_RING3_READY" $deadline $process)
    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_DOOM_MAIN" $deadline $process)
    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_DOOM_DG_INIT" $deadline $process)
    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_OS_DOOM_FRAME_OK" $deadline $process)
    Write-Host "Doom first frame rendered; sending F12..."
    Start-Sleep -Milliseconds 1000
    [void](Invoke-DesktopDoomQmp $qmpReader $qmpWriter @{
        execute = "screendump"
        arguments = @{
            filename = $runtimeScreenshot
            format = "ppm"
        }
    })

    [void](Invoke-DesktopDoomQmp $qmpReader $qmpWriter @{
        execute = "human-monitor-command"
        arguments = @{ "command-line" = "sendkey f12" }
    })
    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_OS_DOOM_INPUT_OK" $deadline $process)
    [void](Wait-DesktopDoomMarker $runtimeSerial "SURA_OS_DOOM_EXIT_OK" $deadline $process)

    try {
        [void](Invoke-DesktopDoomQmp $qmpReader $qmpWriter @{ execute = "quit" })
    }
    catch {
        # QEMU commonly closes the QMP transport before returning the empty
        # response to a successful quit command.
        if (-not $process.WaitForExit(2000)) { throw }
    }
    if (-not $process.WaitForExit(10000)) {
        throw "QEMU did not exit after the completed desktop Doom proof"
    }
    $diagnostics = $stderrTask.GetAwaiter().GetResult()
    if (-not [string]::IsNullOrWhiteSpace($diagnostics)) {
        Write-Verbose $diagnostics
    }
    Copy-Item -LiteralPath $runtimeSerial -Destination $serialLog -Force
    Copy-Item -LiteralPath $runtimeScreenshot -Destination $ScreenshotPath -Force
    $serial = Read-DesktopDoomSerial $serialLog
    "sura_os_doom_desktop_gate: PASS (screenshot=$ScreenshotPath, serial=$serialLog)"
}
catch {
    if ($null -ne $process -and $process.HasExited) {
        Write-Host "QEMU exited unexpectedly with code $($process.ExitCode)"
    }
    if ($null -ne $stderrTask) {
        try {
            $failureDiagnostics = $stderrTask.GetAwaiter().GetResult()
            if (-not [string]::IsNullOrWhiteSpace($failureDiagnostics)) {
                Write-Host $failureDiagnostics
            }
        }
        catch {
        }
    }
    throw
}
finally {
    if (Test-Path -LiteralPath $runtimeSerial -PathType Leaf) {
        Copy-Item -LiteralPath $runtimeSerial -Destination $serialLog -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $qmpWriter) { $qmpWriter.Dispose() }
    if ($null -ne $qmpReader) { $qmpReader.Dispose() }
    if ($null -ne $qmpClient) { $qmpClient.Dispose() }
    if ($null -ne $process) {
        if (-not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        $process.Dispose()
    }
    if (Test-Path -LiteralPath $runtimeDirectory -PathType Container) {
        $resolvedRuntime = [System.IO.Path]::GetFullPath($runtimeDirectory)
        $resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRuntime.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedRuntime).StartsWith("sura_os_doom_desktop_")) {
            Remove-Item -LiteralPath $resolvedRuntime -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
