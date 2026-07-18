param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [string]$Output = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-desktop.ppm"),
    [string]$WindowOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-windows.ppm"),
    [string]$StartOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-start-menu.ppm"),
    [string]$AppsOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-apps.ppm"),
    [string]$BrowserOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser.ppm"),
    [string]$DataDisk = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraData.img"),
    [string]$DataDiskOutput = "",
    [string]$QemuDebugLog = "",
    [int]$TimeoutSeconds = 30,
    [switch]$SkipInputVerification
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$disk = Join-Path $root "build/os/SuraOS.img"
$dataDiskTool = Join-Path $PSScriptRoot "sura_os_data_disk.ps1"

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

function Send-SuraOsMouseMove {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [int]$DeltaX,
        [int]$DeltaY
    )
    [void](Invoke-SuraOsQmp $Reader $Writer @{
        execute = "human-monitor-command"
        arguments = @{ "command-line" = "mouse_move $DeltaX $DeltaY" }
    })
    Start-Sleep -Milliseconds 120
}

function Send-SuraOsMouseButton {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [bool]$Down
    )
    [void](Invoke-SuraOsQmp $Reader $Writer @{
        execute = "input-send-event"
        arguments = @{
            events = @(
                @{ type = "btn"; data = @{ button = "left"; down = $Down } }
            )
        }
    })
    Start-Sleep -Milliseconds 160
}

$qemuProcess = $null
$serialClient = $null
$qmpClient = $null
$serialStream = $null
$qmpReader = $null
$qmpWriter = $null
$temporaryDisk = $null
$temporaryDataDisk = $null
$temporaryCapture = $null
$temporaryWindowCapture = $null
$temporaryStartCapture = $null
$temporaryAppsCapture = $null
$temporaryBrowserCapture = $null

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
    $temporaryDataDisk = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_data.img"
    $temporaryCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$token.ppm"
    $temporaryWindowCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_windows.ppm"
    $temporaryStartCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_start.ppm"
    $temporaryAppsCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_apps.ppm"
    $temporaryBrowserCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser.ppm"
    Copy-Item -LiteralPath $disk -Destination $temporaryDisk -Force
    & $dataDiskTool -Path $DataDisk
    if (-not $?) {
        throw "Sura OS data disk creation failed"
    }
    Copy-Item -LiteralPath $DataDisk -Destination $temporaryDataDisk -Force

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
        "-drive", "file=$temporaryDisk,format=raw,if=ide,index=0",
        "-drive", "file=$temporaryDataDisk,format=raw,if=ide,index=1",
        "-netdev", "user,id=suranet",
        "-device", "virtio-net-pci,netdev=suranet,disable-modern=on,mac=52:54:00:12:34:56",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "c"
    )
    if (-not [string]::IsNullOrWhiteSpace($QemuDebugLog)) {
        $debugDirectory = Split-Path -Parent $QemuDebugLog
        if (-not [string]::IsNullOrWhiteSpace($debugDirectory)) {
            New-Item -ItemType Directory -Path $debugDirectory -Force | Out-Null
        }
        $qemuArguments += @("-d", "int,cpu_reset", "-D", $QemuDebugLog)
    }

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
            -not $serialText.ToString().Contains("SURA_OS_STORAGE_READY") -or
            -not $serialText.ToString().Contains("SURA_OS_STORAGE_READ_OK") -or
            -not $serialText.ToString().Contains("SURA_OS_SETTINGS_READY") -or
            -not $serialText.ToString().Contains("SURA_OS_DESKTOP_STATE_READY") -or
            -not $serialText.ToString().Contains("SURA_OS_DHCP_OK") -or
            -not $serialText.ToString().Contains("SURA_OS_NETWORK_READY") -or
            -not $serialText.ToString().Contains("SURA_OS_ARP_OK") -or
            -not $serialText.ToString().Contains("SURA_OS_UDP_OK") -or
            -not $serialText.ToString().Contains("SURA_OS_DNS_OK") -or
             -not $serialText.ToString().Contains("SURA_OS_TCP_OK") -or
             -not $serialText.ToString().Contains("SURA_OS_HTTP_OK") -or
             -not $serialText.ToString().Contains("SURA_OS_BROWSER_APP_OK") -or
             -not $serialText.ToString().Contains("SURA_OS_BROWSER_CSS_OK") -or
             -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_EDITOR_RING3_READY") -or
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
    if (-not $serialText.ToString().Contains("SURA_OS_STORAGE_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_STORAGE_READ_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_SETTINGS_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_DESKTOP_STATE_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_DHCP_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_NETWORK_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_ARP_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_UDP_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_DNS_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_TCP_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_HTTP_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_BROWSER_APP_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_BROWSER_CSS_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_EDITOR_RING3_READY")) {
        throw "Sura OS storage, persisted desktop state, and VirtIO network were not ready before capture"
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

        # The emulated pointer starts near the screen center. Move to the
        # System Information title bar, focus and drag the window, then close
        # it. Deltas stay inside the PS/2 packet range.
        Send-SuraOsMouseMove $qmpReader $qmpWriter 80 -90
        Send-SuraOsMouseMove $qmpReader $qmpWriter 80 -90
        Send-SuraOsMouseMove $qmpReader $qmpWriter 90 -93
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseMove $qmpReader $qmpWriter 80 70
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $qemuWindowCapturePath = $temporaryWindowCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuWindowCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryWindowCapture -PathType Leaf)) {
            throw "QEMU did not create the managed-window screenshot"
        }
        $windowOutputDirectory = Split-Path -Parent $WindowOutput
        if (-not [string]::IsNullOrWhiteSpace($windowOutputDirectory)) {
            New-Item -ItemType Directory -Path $windowOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryWindowCapture -Destination $WindowOutput -Force
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 0
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 0
        Send-SuraOsMouseMove $qmpReader $qmpWriter 98 0
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false

        # Reopen System Information from its persistent taskbar button.
        for ($step = 0; $step -lt 9; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter -100 0
        }
        for ($step = 0; $step -lt 6; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 0 98
        }
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false

        # Open Start, capture the menu, and select Terminal so keyboard input
        # below is routed to the active terminal window.
        Send-SuraOsMouseMove $qmpReader $qmpWriter -99 0
        Send-SuraOsMouseMove $qmpReader $qmpWriter -99 0
        Send-SuraOsMouseMove $qmpReader $qmpWriter -99 0
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $qemuStartCapturePath = $temporaryStartCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuStartCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryStartCapture -PathType Leaf)) {
            throw "QEMU did not create the Start-menu screenshot"
        }
        $startOutputDirectory = Split-Path -Parent $StartOutput
        if (-not [string]::IsNullOrWhiteSpace($startOutputDirectory)) {
            New-Item -ItemType Directory -Path $startOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryStartCapture -Destination $StartOutput -Force
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 -72
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 -72
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 -72
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 -72
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false

        # Open and exercise the three initial kernel-owned applications.
        for ($step = 0; $step -lt 4; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 0 -88
        }
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        Send-SuraOsMouseMove $qmpReader $qmpWriter 140 160
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        Send-SuraOsMouseMove $qmpReader $qmpWriter -140 22
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        foreach ($key in @("s", "u", "r", "a", "spc", "n", "o", "t", "e", "s")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 80
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 92
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        foreach ($key in @("5", "0", "minus", "3", "1", "equal")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 100
        }
        $qemuAppsCapturePath = $temporaryAppsCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuAppsCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryAppsCapture -PathType Leaf)) {
            throw "QEMU did not create the desktop-apps screenshot"
        }
        $appsOutputDirectory = Split-Path -Parent $AppsOutput
        if (-not [string]::IsNullOrWhiteSpace($appsOutputDirectory)) {
            New-Item -ItemType Directory -Path $appsOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryAppsCapture -Destination $AppsOutput -Force

        # Focus File Explorer from a known pointer origin and open DOCS.
        # This verifies FAT32 subdirectory traversal and the path bar.
        for ($step = 0; $step -lt 14; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter -100 -100
        }
        for ($step = 0; $step -lt 5; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 100 0
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 46 0
        for ($step = 0; $step -lt 7; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 0 100
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 70
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        for ($step = 0; $step -lt 14; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter -100 -100
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 100
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 100
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 100
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 55
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuAppsCapturePath }
        })
        Copy-Item -LiteralPath $temporaryAppsCapture -Destination $AppsOutput -Force

        # Move from a known top-left clamp point to the Browser taskbar
        # button, capture the actual HTTP text window, then focus Terminal.
        for ($step = 0; $step -lt 14; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter -100 -100
        }
        for ($step = 0; $step -lt 8; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 100 0
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 82 0
        for ($step = 0; $step -lt 7; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 0 100
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 70
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false

        $browserFocusDeadline = [DateTime]::UtcNow.AddSeconds(5)
        while ([DateTime]::UtcNow -lt $browserFocusDeadline -and
               -not $serialText.ToString().Contains("SURA_OS_BROWSER_FOCUS_OK")) {
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
        if (-not $serialText.ToString().Contains("SURA_OS_BROWSER_FOCUS_OK")) {
            throw "Sura OS browser taskbar click did not focus the browser.`n$($serialText.ToString())"
        }

        # Replace the address and perform a second live DNS/TCP/HTTP
        # navigation through the graphical browser input path.
        foreach ($key in @("e", "x", "a", "m", "p", "l", "e", "dot", "c", "o", "m", "slash", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 90
        }
        $browserDeadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $browserDeadline -and
               -not $serialText.ToString().Contains("SURA_OS_BROWSER_URL_OK")) {
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
        if (-not $serialText.ToString().Contains("SURA_OS_BROWSER_URL_OK")) {
            throw "Sura OS browser URL input did not complete a live navigation:`n$($serialText.ToString())"
        }
        $qemuBrowserCapturePath = $temporaryBrowserCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserCapture -PathType Leaf)) {
            throw "QEMU did not create the text-browser screenshot"
        }
        $browserOutputDirectory = Split-Path -Parent $BrowserOutput
        if (-not [string]::IsNullOrWhiteSpace($browserOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserCapture -Destination $BrowserOutput -Force

        for ($step = 0; $step -lt 14; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter -100 -100
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 0
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 0
        for ($step = 0; $step -lt 7; $step++) {
            Send-SuraOsMouseMove $qmpReader $qmpWriter 0 100
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 70
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false

        # Fill the graphical terminal through PS/2 so its scroll path
        # executes, then clear it and leave one visible status result.
        for ($round = 0; $round -lt 13; $round++) {
            foreach ($key in @("s", "t", "a", "t", "u", "s", "ret")) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 80
            }
        }
        foreach ($key in @("c", "l", "e", "a", "r", "ret", "s", "t", "a", "t", "u", "s", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 80
        }

        $inputDeadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $inputDeadline -and
               (-not $serialText.ToString().Contains("SURA_OS_KEYBOARD_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SHIFT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_MOUSE_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_MOUSE_CLICK_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_FOCUS_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_DRAG_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_CLOSE_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_REOPEN_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_START_MENU_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_RTC_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_FILES_APP_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_EDITOR_APP_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_APP_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_EDITOR_INPUT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RESULT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_EDITOR_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_EDITOR_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_DIRECTORY_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_STORAGE_WRITE_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_TERMINAL_SCROLL_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CLEAR_OK") -or
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
            "SURA_OS_WINDOW_FOCUS_OK",
            "SURA_OS_WINDOW_DRAG_OK",
            "SURA_OS_WINDOW_CLOSE_OK",
            "SURA_OS_WINDOW_REOPEN_OK",
            "SURA_OS_START_MENU_OK",
            "SURA_OS_RTC_OK",
            "SURA_OS_FILES_APP_OK",
            "SURA_OS_EDITOR_APP_OK",
            "SURA_OS_CALCULATOR_APP_OK",
            "SURA_OS_EDITOR_INPUT_OK",
            "SURA_OS_CALCULATOR_RESULT_OK",
            "SURA_OS_CALCULATOR_RING3_OK",
            "SURA_OS_CALCULATOR_CR3_OK",
            "SURA_OS_EDITOR_RING3_OK",
            "SURA_OS_EDITOR_CR3_OK",
            "SURA_OS_BROWSER_URL_OK",
            "SURA_OS_DIRECTORY_OK",
            "SURA_OS_STORAGE_WRITE_OK",
            "SURA_OS_TERMINAL_SCROLL_OK",
            "SURA_OS_CLEAR_OK",
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

    if (-not $SkipInputVerification) {
        # Open Start again from the Terminal taskbar button and activate its
        # Shut Down entry.
        Send-SuraOsMouseMove $qmpReader $qmpWriter -130 0
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 -55
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
    }
    else {
        $shutdownBytes = [System.Text.Encoding]::ASCII.GetBytes("shutdown`n")
        $serialStream.Write($shutdownBytes, 0, $shutdownBytes.Length)
        $serialStream.Flush()
    }
    $shutdownDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $shutdownDeadline -and -not $qemuProcess.HasExited) {
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not $qemuProcess.HasExited) {
        throw "QEMU did not shut down after the screenshot"
    }
    for ($drain = 0; $drain -lt 20; $drain++) {
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        Start-Sleep -Milliseconds 20
    }
    $expectedExitCode = 35
    if ($SkipInputVerification) { $expectedExitCode = 33 }
    if ($qemuProcess.ExitCode -ne $expectedExitCode) {
        $diagnostics = $qemuStderrTask.GetAwaiter().GetResult()
        throw "QEMU closed with unexpected exit code $($qemuProcess.ExitCode), expected $expectedExitCode`n$diagnostics"
    }
    $dataDiskStatus = "not preserved"
    if (-not [string]::IsNullOrWhiteSpace($DataDiskOutput)) {
        $dataDiskOutputDirectory = Split-Path -Parent $DataDiskOutput
        if (-not [string]::IsNullOrWhiteSpace($dataDiskOutputDirectory)) {
            New-Item -ItemType Directory -Path $dataDiskOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryDataDisk -Destination $DataDiskOutput -Force
        $preservedDataDisk = Get-Item -LiteralPath $DataDiskOutput
        $dataDiskStatus = "$($preservedDataDisk.FullName), $($preservedDataDisk.Length) bytes"
    }
    $capture = Get-Item -LiteralPath $Output
    $inputStatus = "verified"
    if ($SkipInputVerification) { $inputStatus = "skipped" }
    $windowCapture = Get-Item -LiteralPath $WindowOutput -ErrorAction SilentlyContinue
    $windowStatus = "not captured"
    if ($null -ne $windowCapture) {
        $windowStatus = "$($windowCapture.FullName), $($windowCapture.Length) bytes"
    }
    $startCapture = Get-Item -LiteralPath $StartOutput -ErrorAction SilentlyContinue
    $startStatus = "not captured"
    if ($null -ne $startCapture) {
        $startStatus = "$($startCapture.FullName), $($startCapture.Length) bytes"
    }
    $appsCapture = Get-Item -LiteralPath $AppsOutput -ErrorAction SilentlyContinue
    $appsStatus = "not captured"
    if ($null -ne $appsCapture) {
        $appsStatus = "$($appsCapture.FullName), $($appsCapture.Length) bytes"
    }
    $browserCapture = Get-Item -LiteralPath $BrowserOutput -ErrorAction SilentlyContinue
    $browserStatus = "not captured"
    if ($null -ne $browserCapture) {
        $browserStatus = "$($browserCapture.FullName), $($browserCapture.Length) bytes"
    }
    "sura_os_screenshot: PASS (desktop=$($capture.FullName), $($capture.Length) bytes; windows=$windowStatus; start=$startStatus; apps=$appsStatus; browser=$browserStatus; input=$inputStatus; data=$dataDiskStatus)"
}
catch {
    if ($null -ne $serialText -and $serialText.Length -gt 0) {
        Write-Host "Sura OS serial log before failure:"
        Write-Host $serialText.ToString()
    }
    if ($null -ne $qemuProcess -and $qemuProcess.HasExited) {
        Write-Host "QEMU exit code before failure: $($qemuProcess.ExitCode)"
        try {
            $diagnostics = $qemuStderrTask.GetAwaiter().GetResult()
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
    foreach ($temporaryPath in @($temporaryDisk, $temporaryDataDisk, $temporaryCapture, $temporaryWindowCapture, $temporaryStartCapture, $temporaryAppsCapture, $temporaryBrowserCapture)) {
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
