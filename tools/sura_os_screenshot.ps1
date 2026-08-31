param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [string]$Output = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-desktop.ppm"),
    [string]$WindowOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-windows.ppm"),
    [string]$StartOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-start-menu.ppm"),
    [string]$AppsOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-apps.ppm"),
    [string]$KoreanOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-korean-input.ppm"),
    [string]$BrowserOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser.ppm"),
    [string]$BrowserScrolledOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser-scrolled.ppm"),
    [string]$BrowserFormOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser-form.ppm"),
    [string]$BrowserSubmittedOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser-submitted.ppm"),
    [string]$BrowserJavascriptOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser-javascript.ppm"),
    [string]$BrowserWasmOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser-webassembly.ppm"),
    [string]$BrowserKoreanOutput = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraOS-browser-korean.ppm"),
    [string]$DataDisk = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraData.img"),
    [string]$DataDiskOutput = "",
    [string]$QemuDebugLog = "",
    [string]$ExpectedSerialMarker = "",
    [int]$TimeoutSeconds = 30,
    [switch]$SurafsVerificationOnly,
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

function Send-SuraOsKey {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [string]$Key,
        [int]$HoldMilliseconds = 220,
        [int]$AfterMilliseconds = 160
    )
    [void](Invoke-SuraOsQmp $Reader $Writer @{
        execute = "human-monitor-command"
        arguments = @{ "command-line" = "sendkey $Key $HoldMilliseconds" }
    })
    Start-Sleep -Milliseconds $AfterMilliseconds
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

function Move-SuraOsMouseTo {
    param(
        [System.IO.StreamReader]$Reader,
        [System.IO.StreamWriter]$Writer,
        [int]$X,
        [int]$Y
    )
    for ($step = 0; $step -lt 14; $step++) {
        Send-SuraOsMouseMove $Reader $Writer -100 -100
    }
    $remainingX = $X
    $remainingY = $Y
    while ($remainingX -gt 0 -or $remainingY -gt 0) {
        $deltaX = [Math]::Min(100, $remainingX)
        $deltaY = [Math]::Min(100, $remainingY)
        Send-SuraOsMouseMove $Reader $Writer $deltaX $deltaY
        $remainingX -= $deltaX
        $remainingY -= $deltaY
    }
}

function Wait-SuraOsSerialMarkerCount {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [System.Text.StringBuilder]$Text,
        [System.Diagnostics.Process]$Process,
        [string]$Marker,
        [int]$Expected,
        [int]$TimeoutSeconds = 5
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $buffer = New-Object byte[] 4096
    while ([DateTime]::UtcNow -lt $deadline) {
        while ($Stream.DataAvailable) {
            $read = $Stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { break }
            [void]$Text.Append(
                [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            )
        }
        $count = ([regex]::Matches(
            $Text.ToString(),
            [regex]::Escape($Marker)
        )).Count
        if ($count -ge $Expected) { return $true }
        if ($Process.HasExited) { return $false }
        Start-Sleep -Milliseconds 20
    }
    return $false
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
$temporaryKoreanCapture = $null
$temporaryBrowserCapture = $null
$temporaryBrowserScrolledCapture = $null
$temporaryBrowserFormCapture = $null
$temporaryBrowserSubmittedCapture = $null
$temporaryBrowserJavascriptCapture = $null
$temporaryBrowserWasmCapture = $null
$temporaryBrowserKoreanCapture = $null

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
    $temporaryKoreanCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_korean.ppm"
    $temporaryBrowserCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser.ppm"
    $temporaryBrowserScrolledCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser_scrolled.ppm"
    $temporaryBrowserFormCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser_form.ppm"
    $temporaryBrowserSubmittedCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser_submitted.ppm"
    $temporaryBrowserJavascriptCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser_javascript.ppm"
    $temporaryBrowserWasmCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser_webassembly.ppm"
    $temporaryBrowserKoreanCapture = Join-Path ([System.IO.Path]::GetTempPath()) "sura_os_capture_$($token)_browser_korean.ppm"
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
        "-cpu", "max",
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
        "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=1280,yres=800",
        "-device", "qemu-xhci,id=sura-xhci",
        "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
        "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0",
        "-audiodev", "none,id=sura-audio",
        "-device", "ich9-intel-hda,id=sura-hda,msi=off",
        "-device", "hda-output,audiodev=sura-audio",
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
            -not $serialText.ToString().Contains("SURA_OS_SURAFS_READY") -or
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
             -not $serialText.ToString().Contains("SURA_OS_BROWSER_DOM_BOX_OK") -or
             -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_EDITOR_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_FILES_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_TERMINAL_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_SYSTEM_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_BROWSER_RING3_READY") -or
             -not $serialText.ToString().Contains("SURA_OS_USER_SCHEDULER_READY") -or
            (-not [string]::IsNullOrWhiteSpace($ExpectedSerialMarker) -and
             -not $serialText.ToString().Contains($ExpectedSerialMarker)) -or
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
        -not $serialText.ToString().Contains("SURA_OS_SURAFS_READY") -or
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
        -not $serialText.ToString().Contains("SURA_OS_BROWSER_DOM_BOX_OK") -or
        -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_EDITOR_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_FILES_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_TERMINAL_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_SYSTEM_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_BROWSER_RING3_READY") -or
        -not $serialText.ToString().Contains("SURA_OS_USER_SCHEDULER_READY")) {
        throw "Sura OS storage, persisted desktop state, and VirtIO network were not ready before capture"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialMarker) -and
        -not $serialText.ToString().Contains($ExpectedSerialMarker)) {
        throw "Expected serial marker was not observed before capture: $ExpectedSerialMarker"
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

        # Open and exercise the three initial desktop applications.
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
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_EDITOR_APP_OK" 1 3)) {
            Send-SuraOsMouseButton $qmpReader $qmpWriter $true
            Send-SuraOsMouseButton $qmpReader $qmpWriter $false
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_EDITOR_APP_OK" 1 7)) {
                throw "Sura OS Text Editor did not become active before keyboard input"
            }
        }
        # Normalize the shared layout to English before the deterministic
        # editor input sequence, regardless of the preference restored from
        # the supplied data disk.
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $layoutMarkerCount = ([regex]::Matches(
            $serialText.ToString(),
            "SURA_OS_INPUT_LAYOUT_(?:KOREAN|ENGLISH)"
        )).Count
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey alt_r" }
        })
        $layoutToggleDeadline = [DateTime]::UtcNow.AddSeconds(5)
        $newLayoutMarkerCount = $layoutMarkerCount
        while ([DateTime]::UtcNow -lt $layoutToggleDeadline -and
               $newLayoutMarkerCount -le $layoutMarkerCount) {
            while ($serialStream.DataAvailable) {
                $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                if ($read -le 0) { break }
                [void]$serialText.Append(
                    [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                )
            }
            $newLayoutMarkerCount = ([regex]::Matches(
                $serialText.ToString(),
                "SURA_OS_INPUT_LAYOUT_(?:KOREAN|ENGLISH)"
            )).Count
            if ($qemuProcess.HasExited) { break }
            Start-Sleep -Milliseconds 30
        }
        if ($newLayoutMarkerCount -le $layoutMarkerCount) {
            throw "Sura OS did not finish the first editor layout toggle"
        }
        $editorLayoutText = $serialText.ToString()
        if ($editorLayoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN") -gt
            $editorLayoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey alt_r" }
            })
            $englishLayoutDeadline = [DateTime]::UtcNow.AddSeconds(5)
            while ([DateTime]::UtcNow -lt $englishLayoutDeadline -and
                   $serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH") -lt
                   $serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN")) {
                while ($serialStream.DataAvailable) {
                    $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                    if ($read -le 0) { break }
                    [void]$serialText.Append(
                        [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                    )
                }
                if ($qemuProcess.HasExited) { break }
                Start-Sleep -Milliseconds 30
            }
            if ($serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH") -lt
                $serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN")) {
                throw "Sura OS did not return the editor layout to English"
            }
        }
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $asciiEditorEventCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_EDITOR_EVENT_OK sequence=")
        )).Count
        foreach ($key in @("s", "u", "r", "a", "spc", "n", "o", "t", "e", "s", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            $asciiEditorEventCount += 1
            $asciiEditorDeadline = [DateTime]::UtcNow.AddSeconds(5)
            $observedEditorEventCount = 0
            while ([DateTime]::UtcNow -lt $asciiEditorDeadline -and
                   $observedEditorEventCount -lt $asciiEditorEventCount) {
                while ($serialStream.DataAvailable) {
                    $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                    if ($read -le 0) { break }
                    [void]$serialText.Append(
                        [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                    )
                }
                $observedEditorEventCount = ([regex]::Matches(
                    $serialText.ToString(),
                    [regex]::Escape("SURA_OS_EDITOR_EVENT_OK sequence=")
                )).Count
                if ($qemuProcess.HasExited) { break }
                Start-Sleep -Milliseconds 20
            }
            if ($observedEditorEventCount -lt $asciiEditorEventCount) {
                throw "Sura OS editor did not finish ASCII key '$key'"
            }
        }
        # Toggle the Korean layout through the emulated right-Alt key and
        # commit gksrmf ("한글" on a two-set layout) as one UTF-8 mailbox
        # operation per completed composition.
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $editorEventCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_EDITOR_EVENT_OK sequence=")
        )).Count
        foreach ($key in @("alt_r", "g", "k", "s", "r")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 140
        }
        # The r key completes "한" and enters the Ring-3 editor. Wait for that
        # transition and its SuraFS flush before queuing the rest of "글".
        $firstHangulDeadline = [DateTime]::UtcNow.AddSeconds(10)
        $firstHangulCount = $editorEventCount
        while ([DateTime]::UtcNow -lt $firstHangulDeadline -and
               $firstHangulCount -lt $editorEventCount + 1) {
            while ($serialStream.DataAvailable) {
                $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                if ($read -le 0) { break }
                [void]$serialText.Append(
                    [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                )
            }
            $firstHangulCount = ([regex]::Matches(
                $serialText.ToString(),
                [regex]::Escape("SURA_OS_EDITOR_EVENT_OK sequence=")
            )).Count
            if ($qemuProcess.HasExited) { break }
            Start-Sleep -Milliseconds 50
        }
        if ($firstHangulCount -lt $editorEventCount + 1) {
            throw "Sura OS editor did not finish the first Hangul commit"
        }
        foreach ($key in @("m", "f", "spc", "alt_r")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 140
        }
        # The Ring-3 editor transition is asynchronous with respect to QMP
        # key injection. Do not switch focus until the committed UTF-8 bytes
        # have returned to ring 0 and SuraFS has flushed them.
        $koreanEditorDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $koreanEditorDeadline -and
               -not $serialText.ToString().Contains("SURA_OS_KOREAN_INPUT_OK")) {
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
        if (-not $serialText.ToString().Contains("SURA_OS_KOREAN_INPUT_OK")) {
            throw "Sura OS editor did not commit and persist the Korean input before focus changed"
        }
        # The marker is emitted after the UTF-8 bytes are persisted but just
        # before the desktop redraw. Let that redraw finish so the screenshot
        # proves that both completed syllables are visible as well as stored.
        Start-Sleep -Milliseconds 500
        $qemuKoreanCapturePath = $temporaryKoreanCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuKoreanCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryKoreanCapture -PathType Leaf)) {
            throw "QEMU did not create the Korean-input screenshot"
        }
        $koreanOutputDirectory = Split-Path -Parent $KoreanOutput
        if (-not [string]::IsNullOrWhiteSpace($koreanOutputDirectory)) {
            New-Item -ItemType Directory -Path $koreanOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryKoreanCapture -Destination $KoreanOutput -Force
        # Focus Calculator through its fixed taskbar button. Using a relative
        # move from the editor depended on the restored window geometry.
        Move-SuraOsMouseTo $qmpReader $qmpWriter 765 775
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        Start-Sleep -Milliseconds 1000
        $calculatorEventCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_CALCULATOR_EVENT_OK sequence=")
        )).Count
        foreach ($key in @("5", "0", "minus", "3", "1", "equal")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            $calculatorEventCount += 1
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_CALCULATOR_EVENT_OK sequence=" $calculatorEventCount 5)) {
                throw "Sura OS calculator did not finish keyboard key '$key'"
            }
        }
        # Exercise the graphical keypad independently from the keyboard path:
        # C, 7, +, 5, = must produce the exact result 12 through Ring 3.
        Move-SuraOsMouseTo $qmpReader $qmpWriter 915 471
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $calculatorEventCount += 1
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_CALCULATOR_EVENT_OK sequence=" $calculatorEventCount 5)) {
            throw "Sura OS calculator did not finish keypad C"
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 -108
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $calculatorEventCount += 1
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_CALCULATOR_EVENT_OK sequence=" $calculatorEventCount 5)) {
            throw "Sura OS calculator did not finish keypad 7"
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 100 54
        Send-SuraOsMouseMove $qmpReader $qmpWriter 104 54
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $calculatorEventCount += 1
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_CALCULATOR_EVENT_OK sequence=" $calculatorEventCount 5)) {
            throw "Sura OS calculator did not finish keypad +"
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter -68 -36
        Send-SuraOsMouseMove $qmpReader $qmpWriter -68 -36
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $calculatorEventCount += 1
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_CALCULATOR_EVENT_OK sequence=" $calculatorEventCount 5)) {
            throw "Sura OS calculator did not finish keypad 5"
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 68 72
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        $calculatorEventCount += 1
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_CALCULATOR_EVENT_OK sequence=" $calculatorEventCount 5)) {
            throw "Sura OS calculator did not finish keypad ="
        }
        $keypadDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $keypadDeadline -and
               -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_KEYPAD_RESULT_OK")) {
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
        if (-not $serialText.ToString().Contains("SURA_OS_CALCULATOR_KEYPAD_RESULT_OK")) {
            throw "Sura OS graphical calculator keypad did not produce 7 + 5 = 12"
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

        # Focus File Explorer from a known pointer origin, open the first
        # SuraFS directory, then open its first file after the synthetic
        # parent row. This covers UTF-8 navigation and editor activation.
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
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 20
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        if ($SurafsVerificationOnly) {
            # Exercise the Explorer's actual keyboard-driven mutation UI in
            # the mounted Korean document directory.
            $mutationSteps = @(
                @{ Keys = @("f3", "t", "e", "m", "p", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 1 },
                @{ Keys = @("delete", "ret"); Marker = "SURA_OS_SURAFS_TRASH_OK"; Count = 1 },
                @{ Keys = @("f4", "a", "g", "e", "n", "t", "dot", "s", "u", "r", "a", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 2 },
                @{ Keys = @("f2", "r", "e", "n", "a", "m", "e", "d", "dot", "s", "u", "r", "a", "ret"); Marker = "SURA_OS_SURAFS_RENAME_OK"; Count = 1 },
                @{ Keys = @("f4", "alt_r", "g", "k", "s", "r", "m", "f", "alt_r", "dot", "s", "u", "r", "a", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 3 },
                @{ Keys = @("f4", "e", "x", "t", "r", "a", "1", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 4 },
                @{ Keys = @("f4", "e", "x", "t", "r", "a", "2", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 5 },
                @{ Keys = @("f4", "e", "x", "t", "r", "a", "3", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 6 },
                @{ Keys = @("home", "down", "ctrl-c"); Marker = "SURA_OS_FILES_COPY_READY"; Count = 1 },
                @{ Keys = @("ctrl-v"); Marker = "SURA_OS_FILES_PASTE_OK"; Count = 1 },
                @{ Keys = @("ctrl-x"); Marker = "SURA_OS_FILES_CUT_READY"; Count = 1 },
                @{ Keys = @("f3", "m", "o", "v", "e", "b", "o", "x", "ret"); Marker = "SURA_OS_SURAFS_CREATE_OK"; Count = 7 }
            )
            foreach ($mutation in $mutationSteps) {
                foreach ($key in $mutation.Keys) {
                    [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                        execute = "human-monitor-command"
                        arguments = @{ "command-line" = "sendkey $key" }
                    })
                    # File operations and layout changes synchronously commit
                    # SuraFS/FAT32 state while PS/2 remains polling-driven.
                    # Leave enough time for each dependent key to be consumed.
                    Start-Sleep -Milliseconds 320
                }
                $mutationDeadline = [DateTime]::UtcNow.AddSeconds(10)
                $mutationCount = 0
                while ([DateTime]::UtcNow -lt $mutationDeadline -and
                       $mutationCount -lt $mutation.Count) {
                    while ($serialStream.DataAvailable) {
                        $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                        if ($read -le 0) { break }
                        [void]$serialText.Append(
                            [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                        )
                    }
                    $mutationCount = ([regex]::Matches(
                        $serialText.ToString(),
                        [regex]::Escape($mutation.Marker)
                    )).Count
                    if ($qemuProcess.HasExited) { break }
                    Start-Sleep -Milliseconds 30
                }
                if ($mutationCount -lt $mutation.Count) {
                    throw "Sura OS Explorer mutation did not finish: $($mutation.Marker)"
                }
            }

            # Enter movebox, paste the cut file, then return to the parent and
            # duplicate the complete directory tree. This verifies both move
            # semantics and recursive copy through the actual Explorer UI.
            foreach ($key in @("ret", "ctrl-v", "home", "ret", "end", "ctrl-c", "ctrl-v")) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 220
            }
            $clipboardDeadline = [DateTime]::UtcNow.AddSeconds(15)
            while ([DateTime]::UtcNow -lt $clipboardDeadline) {
                while ($serialStream.DataAvailable) {
                    $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                    if ($read -le 0) { break }
                    [void]$serialText.Append(
                        [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                    )
                }
                $clipboardText = $serialText.ToString()
                $copyReadyCount = ([regex]::Matches($clipboardText, "SURA_OS_FILES_COPY_READY")).Count
                $pasteCount = ([regex]::Matches($clipboardText, "SURA_OS_FILES_PASTE_OK")).Count
                if ($copyReadyCount -ge 2 -and $pasteCount -ge 3) { break }
                if ($qemuProcess.HasExited) { break }
                Start-Sleep -Milliseconds 30
            }
            $clipboardText = $serialText.ToString()
            if (([regex]::Matches($clipboardText, "SURA_OS_FILES_COPY_READY")).Count -lt 2 -or
                ([regex]::Matches($clipboardText, "SURA_OS_FILES_CUT_READY")).Count -lt 1 -or
                ([regex]::Matches($clipboardText, "SURA_OS_FILES_PASTE_OK")).Count -lt 3) {
                throw "Sura OS Explorer did not complete file copy, cut/move, and recursive folder copy"
            }

            foreach ($key in @("end", "home")) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 180
            }
            $scrollDeadline = [DateTime]::UtcNow.AddSeconds(5)
            while ([DateTime]::UtcNow -lt $scrollDeadline -and
                   -not $serialText.ToString().Contains("SURA_OS_FILES_SCROLL_OK")) {
                while ($serialStream.DataAvailable) {
                    $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                    if ($read -le 0) { break }
                    [void]$serialText.Append(
                        [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                    )
                }
                if ($qemuProcess.HasExited) { break }
                Start-Sleep -Milliseconds 30
            }
            if (-not $serialText.ToString().Contains("SURA_OS_FILES_SCROLL_OK")) {
                throw "Sura OS Explorer did not scroll beyond its six-row viewport"
            }
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 0 34
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        if ($SurafsVerificationOnly) {
            $openDeadline = [DateTime]::UtcNow.AddSeconds(10)
            while ([DateTime]::UtcNow -lt $openDeadline -and
                   -not $serialText.ToString().Contains("SURA_OS_SURAFS_FILE_OPEN_OK")) {
                while ($serialStream.DataAvailable) {
                    $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                    if ($read -le 0) { break }
                    [void]$serialText.Append(
                        [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                    )
                }
                if ($qemuProcess.HasExited) { break }
                Start-Sleep -Milliseconds 30
            }
            if (-not $serialText.ToString().Contains("SURA_OS_SURAFS_FILE_OPEN_OK")) {
                throw "Sura OS Explorer did not finish opening the document before Save As"
            }
            $editorFindCount = ([regex]::Matches(
                $serialText.ToString(),
                [regex]::Escape("SURA_OS_EDITOR_FIND_OK")
            )).Count
            foreach ($key in @("ctrl-f", "shift-s", "u", "r", "a", "ret")) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 320
            }
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_EDITOR_FIND_OK" ($editorFindCount + 1) 10)) {
                throw "Sura OS Text Editor did not complete find"
            }

            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey esc" }
            })
            Start-Sleep -Milliseconds 500

            $editorReplaceCount = ([regex]::Matches(
                $serialText.ToString(),
                [regex]::Escape("SURA_OS_EDITOR_REPLACE_OK")
            )).Count
            $editorEventCount = ([regex]::Matches(
                $serialText.ToString(),
                [regex]::Escape("SURA_OS_EDITOR_EVENT_OK sequence=")
            )).Count
            foreach ($key in @(
                "ctrl-h", "shift-s", "u", "r", "a", "tab",
                "shift-s", "u", "r", "a", "ret"
            )) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 320
            }
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_EDITOR_REPLACE_OK" ($editorReplaceCount + 1) 10)) {
                throw "Sura OS Text Editor did not complete replace"
            }
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_EDITOR_EVENT_OK sequence=" ($editorEventCount + 1) 10)) {
                throw "Sura OS Text Editor Ring-3 replace did not return"
            }
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey esc" }
            })
            Start-Sleep -Milliseconds 220
            foreach ($key in @("ctrl-a", "ctrl-c", "ctrl-x", "ctrl-v")) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 260
            }
            $editorClipboardDeadline = [DateTime]::UtcNow.AddSeconds(10)
            while ([DateTime]::UtcNow -lt $editorClipboardDeadline -and
                   (-not $serialText.ToString().Contains("SURA_OS_EDITOR_SELECTION_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_COPY_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_CUT_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_PASTE_OK"))) {
                while ($serialStream.DataAvailable) {
                    $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                    if ($read -le 0) { break }
                    [void]$serialText.Append(
                        [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                    )
                }
                if ($qemuProcess.HasExited) { break }
                Start-Sleep -Milliseconds 30
            }
            foreach ($marker in @(
                "SURA_OS_EDITOR_SELECTION_OK",
                "SURA_OS_EDITOR_COPY_OK",
                "SURA_OS_EDITOR_CUT_OK",
                "SURA_OS_EDITOR_PASTE_OK"
            )) {
                if (-not $serialText.ToString().Contains($marker)) {
                    throw "Sura OS Text Editor clipboard verification did not observe: $marker"
                }
            }
            $saveAsCount = ([regex]::Matches(
                $serialText.ToString(),
                [regex]::Escape("SURA_OS_SURAFS_SAVE_AS_OK")
            )).Count
            $syntaxCount = ([regex]::Matches(
                $serialText.ToString(),
                [regex]::Escape("SURA_OS_EDITOR_SYNTAX_OK")
            )).Count
            foreach ($key in @("ctrl-shift-s", "c", "o", "p", "y", "dot", "s", "u", "r", "a", "ret")) {
                [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                    execute = "human-monitor-command"
                    arguments = @{ "command-line" = "sendkey $key" }
                })
                Start-Sleep -Milliseconds 240
            }
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_SURAFS_SAVE_AS_OK" ($saveAsCount + 1) 10)) {
                throw "Sura OS Text Editor Save As did not finish before opening code"
            }

            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_EDITOR_SYNTAX_OK" ($syntaxCount + 1) 10)) {
                throw "Sura OS did not activate syntax highlighting for copy.sura"
            }

        }
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuAppsCapturePath }
        })
        Copy-Item -LiteralPath $temporaryAppsCapture -Destination $AppsOutput -Force

        if ($SurafsVerificationOnly) {
            $surafsDeadline = [DateTime]::UtcNow.AddSeconds(10)
            while ([DateTime]::UtcNow -lt $surafsDeadline -and
                   (-not $serialText.ToString().Contains("SURA_OS_SURAFS_EDITOR_SAVE_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_DIRECTORY_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_SURAFS_FILE_OPEN_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_SURAFS_CREATE_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_SURAFS_RENAME_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_SURAFS_TRASH_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_FILES_SCROLL_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_FILES_COPY_READY") -or
                    -not $serialText.ToString().Contains("SURA_OS_FILES_CUT_READY") -or
                    -not $serialText.ToString().Contains("SURA_OS_FILES_PASTE_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_SELECTION_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_COPY_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_CUT_OK") -or
                    -not $serialText.ToString().Contains("SURA_OS_EDITOR_PASTE_OK") -or
                     -not $serialText.ToString().Contains("SURA_OS_EDITOR_FIND_OK") -or
                     -not $serialText.ToString().Contains("SURA_OS_EDITOR_REPLACE_OK") -or
                     -not $serialText.ToString().Contains("SURA_OS_EDITOR_SYNTAX_OK") -or
                     -not $serialText.ToString().Contains("SURA_OS_SURAFS_SAVE_AS_OK"))) {
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
                "SURA_OS_SURAFS_READY",
                "SURA_OS_SURAFS_EDITOR_SAVE_OK",
                "SURA_OS_DIRECTORY_OK",
                "SURA_OS_SURAFS_FILE_OPEN_OK",
                "SURA_OS_SURAFS_CREATE_OK",
                "SURA_OS_SURAFS_RENAME_OK",
                "SURA_OS_SURAFS_TRASH_OK",
                "SURA_OS_FILES_SCROLL_OK",
                "SURA_OS_FILES_COPY_READY",
                "SURA_OS_FILES_CUT_READY",
                "SURA_OS_FILES_PASTE_OK",
                "SURA_OS_EDITOR_SELECTION_OK",
                "SURA_OS_EDITOR_COPY_OK",
                "SURA_OS_EDITOR_CUT_OK",
                "SURA_OS_EDITOR_PASTE_OK",
                "SURA_OS_EDITOR_FIND_OK",
                "SURA_OS_EDITOR_REPLACE_OK",
                "SURA_OS_EDITOR_SYNTAX_OK",
                "SURA_OS_SURAFS_SAVE_AS_OK"
            )) {
                if (-not $serialText.ToString().Contains($marker)) {
                    throw "SuraFS GUI verification did not observe: $marker"
                }
            }

            $shutdownBytes = [System.Text.Encoding]::ASCII.GetBytes("shutdown`n")
            $serialStream.Write($shutdownBytes, 0, $shutdownBytes.Length)
            $serialStream.Flush()
            if (-not $qemuProcess.WaitForExit(10000)) {
                throw "QEMU did not shut down after SuraFS GUI verification"
            }
            if ($qemuProcess.ExitCode -ne 0) {
                throw "SuraFS GUI verification closed with unexpected exit code $($qemuProcess.ExitCode), expected ACPI power-off exit 0"
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
            "sura_os_screenshot: SURAFS GUI PASS (data=$dataDiskStatus)"
            return
        }

        # Move from a known top-left clamp point to the Browser taskbar
        # button, capture the actual HTTP text window, then focus Terminal.
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $browserFocusCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_FOCUS_OK")
        )).Count
        Move-SuraOsMouseTo $qmpReader $qmpWriter 882 770
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false

        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_FOCUS_OK" ($browserFocusCount + 1) 3)) {
            # Re-clamp and move again in case a PS/2 relative-motion packet
            # was dropped while other GUI verification was still redrawing.
            Move-SuraOsMouseTo $qmpReader $qmpWriter 882 770
            Send-SuraOsMouseButton $qmpReader $qmpWriter $true
            Send-SuraOsMouseButton $qmpReader $qmpWriter $false
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_BROWSER_FOCUS_OK" ($browserFocusCount + 1) 7)) {
                throw "Sura OS browser taskbar click did not focus the browser.`n$($serialText.ToString())"
            }
        }

        # The editor composition test can leave the shared layout in Korean
        # if the final emulated modifier event is delayed. Normalize it to
        # English before entering the ASCII address.
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $layoutText = $serialText.ToString()
        if ($layoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN") -gt
            $layoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey alt_r" }
            })
            Start-Sleep -Milliseconds 250
        }

        # Start a real DNS query for a reserved invalid TLD, then press F6
        # while that query is pending. The Browser must process the key,
        # cancel the stale address snapshot, and remain usable instead of
        # blocking inside the old monolithic DNS spin loop.
        $browserAsyncBeginCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_ASYNC_BEGIN")
        )).Count
        $browserAsyncInputCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_ASYNC_INPUT_OK")
        )).Count
        foreach ($key in @("n", "o", "a", "n", "s", "w", "e", "r", "dot", "i", "n", "v", "a", "l", "i", "d", "slash", "ret")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_ASYNC_BEGIN" ($browserAsyncBeginCount + 1) 5)) {
            throw "Sura OS browser did not start the incremental DNS navigation unit:`n$($serialText.ToString())"
        }
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 220 250
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_ASYNC_INPUT_OK" ($browserAsyncInputCount + 1) 5)) {
            throw "Sura OS browser did not process keyboard input while DNS was pending:`n$($serialText.ToString())"
        }

        # A literal RFC 5737 TEST-NET address bypasses DNS and leaves the TCP
        # SYN pending. F6 must still be delivered by the desktop loop and
        # cancel that connection attempt without entering the fetch buffers.
        $browserTcpBeginCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TCP_BEGIN")
        )).Count
        $browserTcpInputCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TCP_INPUT_OK")
        )).Count
        foreach ($key in @("2", "0", "3", "dot", "0", "dot", "1", "1", "3", "dot", "1", "slash", "ret")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TCP_BEGIN" ($browserTcpBeginCount + 1) 5)) {
            throw "Sura OS browser did not start the incremental TCP connect unit:`n$($serialText.ToString())"
        }
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 220 250
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TCP_INPUT_OK" ($browserTcpInputCount + 1) 5)) {
            throw "Sura OS browser did not process keyboard input while TCP connect was pending:`n$($serialText.ToString())"
        }

        # Replace the cancelled address and perform a second live
        # DNS/TCP/HTTP navigation through the graphical browser input path.
        $browserAsyncDoneCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_ASYNC_DONE")
        )).Count
        $browserTcpDoneCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TCP_DONE")
        )).Count
        foreach ($key in @("h", "t", "t", "p", "shift-semicolon", "slash", "slash", "e", "x", "a", "m", "p", "l", "e", "dot", "o", "r", "g", "slash", "ret")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key
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
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_ASYNC_DONE" ($browserAsyncDoneCount + 1) 5)) {
            throw "Sura OS browser did not finish the incremental DNS navigation unit:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TCP_DONE" ($browserTcpDoneCount + 1) 5)) {
            throw "Sura OS browser did not finish the incremental TCP connect unit:`n$($serialText.ToString())"
        }
        $domBoxCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_DOM_BOX_OK")
        )).Count
        $externalCssCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_EXTERNAL_CSS_OK")
        )).Count
        $cssVariablesCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_CSS_VARIABLES_OK")
        )).Count
        $cssPositionCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_CSS_POSITION_OK")
        )).Count
        $domRenderCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_DOM_RENDER_OK")
        )).Count
        $imagePngCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_IMAGE_PNG_OK")
        )).Count
        $imageRenderCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_IMAGE_RENDER_OK")
        )).Count
        $browserFetchBeginCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_FETCH_BEGIN")
        )).Count
        $browserFetchInputCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_FETCH_INPUT_OK")
        )).Count
        $browserFetchCancelRequestCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED")
        )).Count
        $browserFetchCancelledCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK")
        )).Count
        $browserTlsBeginCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TLS_BEGIN")
        )).Count
        $browserTlsDoneCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TLS_DONE")
        )).Count
        # Prove an actual incremental TLS 1.3 handshake, certificate
        # validation, encrypted HTTP response, and browser render against the
        # project's live production domain. First cancel one synchronous
        # response fetch with F6 and require the shared network stack to unwind
        # cleanly. Then navigate again and prove cooperative pointer delivery
        # while the remaining response/resource work is pending.
        foreach ($key in @("s", "u", "r", "a", "l", "a", "n", "g", "dot", "s", "i", "t", "e", "slash", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 220
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TLS_BEGIN" ($browserTlsBeginCount + 1) 15)) {
            throw "suralang.site did not begin the incremental TLS handshake:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TLS_DONE" ($browserTlsDoneCount + 1) 20)) {
            throw "suralang.site did not complete the incremental TLS handshake:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_FETCH_BEGIN" ($browserFetchBeginCount + 1) 15)) {
            throw "suralang.site did not enter the cooperative HTTP/TLS fetch phase:`n$($serialText.ToString())"
        }
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 80 50
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED" ($browserFetchCancelRequestCount + 1) 10)) {
            throw "Sura OS did not accept F6 as a fetch cancellation request:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK" ($browserFetchCancelledCount + 1) 10)) {
            throw "Sura OS did not unwind and restore the browser after fetch cancellation:`n$($serialText.ToString())"
        }

        $browserFetchBeginCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_FETCH_BEGIN")
        )).Count
        $browserFetchInputCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_FETCH_INPUT_OK")
        )).Count
        $browserTlsBeginCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TLS_BEGIN")
        )).Count
        $browserTlsDoneCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_NAV_TLS_DONE")
        )).Count
        foreach ($key in @("s", "u", "r", "a", "l", "a", "n", "g", "dot", "s", "i", "t", "e", "slash", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 220
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TLS_BEGIN" ($browserTlsBeginCount + 1) 15)) {
            throw "suralang.site retry did not begin the incremental TLS handshake:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_TLS_DONE" ($browserTlsDoneCount + 1) 20)) {
            throw "suralang.site retry did not complete the incremental TLS handshake:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_FETCH_BEGIN" ($browserFetchBeginCount + 1) 15)) {
            throw "suralang.site retry did not enter the cooperative HTTP/TLS fetch phase:`n$($serialText.ToString())"
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 24 0
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_NAV_FETCH_INPUT_OK" ($browserFetchInputCount + 1) 10)) {
            throw "Sura OS did not process pointer input while the suralang.site HTTP/TLS fetch was pending:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_HTTPS_OK" 1 60)) {
            throw "suralang.site did not complete the live HTTPS browser gate:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_DOM_BOX_OK" ($domBoxCount + 1) 10)) {
            throw "suralang.site did not complete DOM/computed-style/box layout:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_EXTERNAL_CSS_OK" ($externalCssCount + 1) 10)) {
            throw "suralang.site external stylesheet was not fetched and decoded:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_CSS_VARIABLES_OK" ($cssVariablesCount + 1) 10)) {
            throw "suralang.site :root custom properties were not resolved:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_CSS_POSITION_OK" ($cssPositionCount + 1) 10)) {
            throw "suralang.site positioned boxes were not computed:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_DOM_RENDER_OK" ($domRenderCount + 1) 60)) {
            throw "suralang.site DOM boxes were not painted into the browser window:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_IMAGE_PNG_OK" ($imagePngCount + 1) 10)) {
            throw "suralang.site PNG image was not fetched and decoded:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_IMAGE_RENDER_OK" ($imageRenderCount + 1) 10)) {
            throw "suralang.site decoded image was not painted into the browser window:`n$($serialText.ToString())"
        }
        Start-Sleep -Milliseconds 750
        $qemuBrowserCapturePath = $temporaryBrowserCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserCapture -PathType Leaf)) {
            throw "QEMU did not create the HTTPS text-browser screenshot"
        }
        $browserOutputDirectory = Split-Path -Parent $BrowserOutput
        if (-not [string]::IsNullOrWhiteSpace($browserOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserCapture -Destination $BrowserOutput -Force

        # Click the first visible navigation anchor in the live page. This
        # verifies that DOM box hit-testing reaches the owning <a> element and
        # that a same-document fragment updates the viewport.
        $browserLinkCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_LINK_OK")
        )).Count
        # The saved desktop may restore the browser 40 pixels to the right of
        # its default position. This point remains inside the text/content
        # area of the live skip-navigation anchor in both geometries, including
        # when compound border widths change its outer box.
        Move-SuraOsMouseTo $qmpReader $qmpWriter 155 166
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_LINK_OK" ($browserLinkCount + 1) 5)) {
            # Re-clamp and use a second point farther inside the same visible
            # header anchor. Relative HID motion can lose one packet while a
            # large live-page redraw is settling.
            Move-SuraOsMouseTo $qmpReader $qmpWriter 190 166
            Send-SuraOsMouseButton $qmpReader $qmpWriter $true
            Send-SuraOsMouseButton $qmpReader $qmpWriter $false
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_BROWSER_LINK_OK" ($browserLinkCount + 1) 5)) {
                throw "Sura OS browser DOM link click did not activate the target anchor:`n$($serialText.ToString())"
            }
        }

        # QEMU's USB mouse runs in HID report protocol so the fourth report
        # byte carries the wheel delta into PointerEvent.
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "input-send-event"
            arguments = @{
                events = @(
                    @{ type = "btn"; data = @{ button = "wheel-down"; down = $true } }
                )
            }
        })
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_WHEEL_OK" 1 10)) {
            throw "Sura OS browser did not receive a USB mouse-wheel PointerEvent:`n$($serialText.ToString())"
        }

        # Hold Page Down long enough to prove both browser scrolling and the
        # device-independent OS key-repeat generator, then preserve the
        # resulting viewport separately from the top-of-page capture.
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey pgdn 900" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_SCROLL_OK" 1 10)) {
            throw "Sura OS browser Page Down did not scroll the DOM viewport:`n$($serialText.ToString())"
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_KEY_REPEAT_OK" 1 10)) {
            throw "Sura OS did not generate held-key repeat events:`n$($serialText.ToString())"
        }
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "input-send-event"
            arguments = @{
                events = @(
                    @{
                        type = "key"
                        data = @{
                            down = $false
                            key = @{ type = "qcode"; data = "pgdn" }
                        }
                    }
                )
            }
        })
        # The QEMU HMP hold time is asynchronous: the repeat marker can arrive
        # before the scheduled 900 ms key release. Wait past that release so
        # the following address-bar text cannot be dropped behind Page Down
        # repeat events in the bounded guest input queue.
        Start-Sleep -Milliseconds 1100
        $qemuBrowserScrolledCapturePath = $temporaryBrowserScrolledCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserScrolledCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserScrolledCapture -PathType Leaf)) {
            throw "QEMU did not create the scrolled browser screenshot"
        }
        $browserScrolledOutputDirectory = Split-Path -Parent $BrowserScrolledOutput
        if (-not [string]::IsNullOrWhiteSpace($browserScrolledOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserScrolledOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserScrolledCapture -Destination $BrowserScrolledOutput -Force

        # Load the deterministic built-in form page through the real
        # browser DOM/CSS/box path, focus its first input, and append "ra" to
        # the initial "su" value. This proves page-control focus is distinct
        # from address-bar editing.
        $browserFormPageCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_FORM_PAGE_OK")
        )).Count
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 220 250
        foreach ($key in @("s", "u", "r", "a", "dot", "l", "o", "c", "a", "l", "slash", "f", "o", "r", "m", "s", "ret")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_FORM_PAGE_OK" ($browserFormPageCount + 1) 10)) {
            throw "Sura OS built-in browser form page did not load:`n$($serialText.ToString())"
        }
        $browserFormFocusCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_FORM_FOCUS_OK")
        )).Count
        # Keep a pre-focus capture as useful failure evidence. A successful
        # run overwrites this path after editing the form value below.
        $qemuBrowserFormCapturePath = $temporaryBrowserFormCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserFormCapturePath }
        })
        if (Test-Path -LiteralPath $temporaryBrowserFormCapture -PathType Leaf) {
            $browserFormOutputDirectory = Split-Path -Parent $BrowserFormOutput
            if (-not [string]::IsNullOrWhiteSpace($browserFormOutputDirectory)) {
                New-Item -ItemType Directory -Path $browserFormOutputDirectory -Force | Out-Null
            }
            Copy-Item -LiteralPath $temporaryBrowserFormCapture -Destination $BrowserFormOutput -Force
        }
        Move-SuraOsMouseTo $qmpReader $qmpWriter 180 238
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_FORM_FOCUS_OK" ($browserFormFocusCount + 1) 5)) {
            throw "Sura OS browser form input did not receive pointer focus:`n$($serialText.ToString())"
        }
        $browserFormInputCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_FORM_INPUT_OK")
        )).Count
        foreach ($key in @("r", "a")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 180
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_FORM_INPUT_OK" ($browserFormInputCount + 1) 5)) {
            throw "Sura OS browser form input did not edit its mutable value:`n$($serialText.ToString())"
        }
        Start-Sleep -Milliseconds 250
        $qemuBrowserFormCapturePath = $temporaryBrowserFormCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserFormCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserFormCapture -PathType Leaf)) {
            throw "QEMU did not create the browser-form screenshot"
        }
        $browserFormOutputDirectory = Split-Path -Parent $BrowserFormOutput
        if (-not [string]::IsNullOrWhiteSpace($browserFormOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserFormOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserFormCapture -Destination $BrowserFormOutput -Force

        # Enter submits the focused control's parent form. The internal page
        # uses method=post and the same application/x-www-form-urlencoded
        # serializer/request path as live pages, then renders its submitted
        # payload on a deterministic success page.
        $browserFormSubmitCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_FORM_SUBMIT_OK")
        )).Count
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey ret" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_FORM_SUBMIT_OK" ($browserFormSubmitCount + 1) 10)) {
            throw "Sura OS browser form did not serialize and submit its POST payload:`n$($serialText.ToString())"
        }
        Start-Sleep -Milliseconds 250
        $qemuBrowserSubmittedCapturePath = $temporaryBrowserSubmittedCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserSubmittedCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserSubmittedCapture -PathType Leaf)) {
            throw "QEMU did not create the submitted-form browser screenshot"
        }
        $browserSubmittedOutputDirectory = Split-Path -Parent $BrowserSubmittedOutput
        if (-not [string]::IsNullOrWhiteSpace($browserSubmittedOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserSubmittedOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserSubmittedCapture -Destination $BrowserSubmittedOutput -Force

        # Load a deterministic JavaScript page, click its real DOM button, and
        # require the inline onclick handler to update #status.textContent.
        # The first capture is retained as failure evidence if hit-testing or
        # execution fails; a successful click overwrites it with the mutated
        # DOM rendering.
        # F6 selects the browser address field without depending on pointer
        # rounding at the edge of the maximized window.
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 220 250
        $browserJavascriptPageCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_JS_PAGE_OK")
        )).Count
        foreach ($key in @("s", "u", "r", "a", "dot", "l", "o", "c", "a", "l", "slash", "j", "a", "v", "a", "s", "c", "r", "i", "p", "t", "ret")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key
        }
        $qemuBrowserJavascriptCapturePath = $temporaryBrowserJavascriptCapture.Replace('\', '/')
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_JS_PAGE_OK" ($browserJavascriptPageCount + 1) 10)) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "screendump"
                arguments = @{ filename = $qemuBrowserJavascriptCapturePath }
            })
            if (Test-Path -LiteralPath $temporaryBrowserJavascriptCapture -PathType Leaf) {
                $browserJavascriptOutputDirectory = Split-Path -Parent $BrowserJavascriptOutput
                if (-not [string]::IsNullOrWhiteSpace($browserJavascriptOutputDirectory)) {
                    New-Item -ItemType Directory -Path $browserJavascriptOutputDirectory -Force | Out-Null
                }
                Copy-Item -LiteralPath $temporaryBrowserJavascriptCapture -Destination $BrowserJavascriptOutput -Force
            }
            throw "Sura OS built-in JavaScript page did not load:`n$($serialText.ToString())"
        }
        Start-Sleep -Milliseconds 250
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserJavascriptCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserJavascriptCapture -PathType Leaf)) {
            throw "QEMU did not create the JavaScript browser screenshot"
        }
        $browserJavascriptOutputDirectory = Split-Path -Parent $BrowserJavascriptOutput
        if (-not [string]::IsNullOrWhiteSpace($browserJavascriptOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserJavascriptOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserJavascriptCapture -Destination $BrowserJavascriptOutput -Force

        $browserJavascriptClickCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_JS_CLICK_OK")
        )).Count
        Move-SuraOsMouseTo $qmpReader $qmpWriter 180 260
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_JS_CLICK_OK" ($browserJavascriptClickCount + 1) 10)) {
            throw "Sura OS browser JavaScript onclick handler did not execute:`n$($serialText.ToString())"
        }
        Start-Sleep -Milliseconds 250
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserJavascriptCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserJavascriptCapture -PathType Leaf)) {
            throw "QEMU did not create the mutated JavaScript browser screenshot"
        }
        Copy-Item -LiteralPath $temporaryBrowserJavascriptCapture -Destination $BrowserJavascriptOutput -Force

        # Load a real binary WebAssembly module through the browser path.
        # Its exported run() function returns 42; the OS runtime executes it
        # under fixed table, stack, call-depth, and instruction limits and
        # renders the result as a DOM page.
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 220 250
        $browserWasmPageCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_WASM_PAGE_OK")
        )).Count
        foreach ($key in @("s", "u", "r", "a", "dot", "l", "o", "c", "a", "l", "slash", "w", "e", "b", "a", "s", "s", "e", "m", "b", "l", "y", "ret")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key
        }
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_WASM_PAGE_OK" ($browserWasmPageCount + 1) 10)) {
            throw "Sura OS built-in WebAssembly page did not execute:`n$($serialText.ToString())"
        }
        Start-Sleep -Milliseconds 250
        $qemuBrowserWasmCapturePath = $temporaryBrowserWasmCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserWasmCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserWasmCapture -PathType Leaf)) {
            throw "QEMU did not create the WebAssembly browser screenshot"
        }
        $browserWasmOutputDirectory = Split-Path -Parent $BrowserWasmOutput
        if (-not [string]::IsNullOrWhiteSpace($browserWasmOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserWasmOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserWasmCapture -Destination $BrowserWasmOutput -Force

        # Enter a UTF-8 Korean path and capture the Unicode address bar
        # independently from the live ASCII navigation above. A second live
        # request is deliberately avoided so DNS timing cannot make the input
        # and rendering gate flaky.
        Send-SuraOsKey $qmpReader $qmpWriter "f6" 220 250
        foreach ($key in @("e", "x", "a", "m", "p", "l", "e", "dot", "c", "o", "m", "slash", "alt_r", "g", "k", "s", "r", "m", "f", "alt_r")) {
            Send-SuraOsKey $qmpReader $qmpWriter $key 220 260
        }
        $utf8BrowserDeadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $utf8BrowserDeadline -and
               -not $serialText.ToString().Contains("SURA_OS_BROWSER_KOREAN_INPUT_OK")) {
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
        if (-not $serialText.ToString().Contains("SURA_OS_BROWSER_KOREAN_INPUT_OK")) {
            throw "Sura OS UTF-8 browser path did not complete:`n$($serialText.ToString())"
        }
        $qemuBrowserKoreanCapturePath = $temporaryBrowserKoreanCapture.Replace('\', '/')
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "screendump"
            arguments = @{ filename = $qemuBrowserKoreanCapturePath }
        })
        if (-not (Test-Path -LiteralPath $temporaryBrowserKoreanCapture -PathType Leaf)) {
            throw "QEMU did not create the Korean browser screenshot"
        }
        $browserKoreanOutputDirectory = Split-Path -Parent $BrowserKoreanOutput
        if (-not [string]::IsNullOrWhiteSpace($browserKoreanOutputDirectory)) {
            New-Item -ItemType Directory -Path $browserKoreanOutputDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $temporaryBrowserKoreanCapture -Destination $BrowserKoreanOutput -Force

        # Hold Backspace on the populated UTF-8 address. The browser emits its
        # dedicated marker only after a repeated Backspace event deletes a
        # complete code point, proving this path rather than generic key repeat.
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            # Keep the key down well past the guest repeat delay. A 600 ms
            # hold was too close to the threshold under slower TCG runs.
            arguments = @{ "command-line" = "sendkey backspace 1200" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount `
            $serialStream $serialText $qemuProcess `
            "SURA_OS_BROWSER_BACKSPACE_REPEAT_OK" 1 10)) {
            throw "Sura OS browser held Backspace did not repeat over the UTF-8 address:`n$($serialText.ToString())"
        }
        # The repeat marker is emitted while QEMU is still holding the key.
        # Wait for the release report and the browser's coalesced final redraw
        # before moving focus to Terminal.
        Start-Sleep -Milliseconds 1000

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

        # The pointer is now over the Terminal taskbar button and Terminal is
        # active. Normalize the shared input layout and wait for the actual
        # guest-side toggle before sending the physical Korean key sequence.
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $terminalLayoutText = $serialText.ToString()
        if ($terminalLayoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN") -le
            $terminalLayoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH")) {
            $terminalKoreanCount = ([regex]::Matches(
                $terminalLayoutText,
                [regex]::Escape("SURA_OS_INPUT_LAYOUT_KOREAN")
            )).Count
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey alt_r" }
            })
            if (-not (Wait-SuraOsSerialMarkerCount `
                $serialStream $serialText $qemuProcess `
                "SURA_OS_INPUT_LAYOUT_KOREAN" ($terminalKoreanCount + 1) 5)) {
                throw "Sura OS Terminal did not switch to the Korean input layout"
            }
            # The marker is emitted on the modifier key-down path. Let QEMU
            # deliver Alt_R key-up before the first composition key.
            Start-Sleep -Milliseconds 250
        }
        foreach ($key in @("d", "k", "s", "s", "u", "d", "g", "k", "t", "p", "d", "y", "ret")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey $key" }
            })
            Start-Sleep -Milliseconds 80
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_TERMINAL_KOREAN_INPUT_OK" 1 10)) {
            throw "Sura OS Terminal did not compose the expected Korean phrase"
        }

        $inputDeadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $inputDeadline -and
               (-not $serialText.ToString().Contains("SURA_OS_KEYBOARD_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_SERVER_RING3_READY") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_SERVER_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_SERVER_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_INPUT_EVENT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_INPUT_LAYOUT_OK") -or
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
                -not $serialText.ToString().Contains("SURA_OS_KOREAN_INPUT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RESULT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_KEYPAD_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_KEYPAD_RESULT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_CALCULATOR_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_EDITOR_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_EDITOR_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_FILES_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_FILES_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_TERMINAL_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_TERMINAL_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SYSTEM_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SYSTEM_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_BROWSER_RING3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_BROWSER_CR3_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_USER_PROCESSES_PERSISTENT_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SYSTEM_APP_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_DIRECTORY_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_STORAGE_WRITE_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SURAFS_EDITOR_SAVE_OK") -or
                -not $serialText.ToString().Contains("SURA_OS_SURAFS_FILE_OPEN_OK") -or
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
            "SURA_OS_WINDOW_SERVER_RING3_READY",
            "SURA_OS_WINDOW_SERVER_RING3_OK",
            "SURA_OS_WINDOW_SERVER_CR3_OK",
            "SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK",
            "SURA_OS_INPUT_EVENT_OK",
            "SURA_OS_INPUT_LAYOUT_OK",
            "SURA_OS_INPUT_LAYOUT_ENGLISH",
            "SURA_OS_INPUT_LAYOUT_KOREAN",
            "SURA_OS_XHCI_INPUT_READY",
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
            "SURA_OS_KOREAN_INPUT_OK",
            "SURA_OS_TERMINAL_KOREAN_INPUT_OK",
            "SURA_OS_CALCULATOR_RESULT_OK",
            "SURA_OS_CALCULATOR_KEYPAD_OK",
            "SURA_OS_CALCULATOR_KEYPAD_RESULT_OK",
            "SURA_OS_CALCULATOR_RING3_OK",
            "SURA_OS_CALCULATOR_CR3_OK",
            "SURA_OS_EDITOR_RING3_OK",
            "SURA_OS_EDITOR_CR3_OK",
            "SURA_OS_FILES_RING3_OK",
            "SURA_OS_FILES_CR3_OK",
            "SURA_OS_TERMINAL_RING3_OK",
            "SURA_OS_TERMINAL_CR3_OK",
            "SURA_OS_SYSTEM_RING3_OK",
            "SURA_OS_SYSTEM_CR3_OK",
            "SURA_OS_BROWSER_RING3_READY",
            "SURA_OS_BROWSER_RING3_OK",
            "SURA_OS_BROWSER_CR3_OK",
            "SURA_OS_USER_PROCESSES_PERSISTENT_OK",
            "SURA_OS_SYSTEM_APP_OK",
            "SURA_OS_BROWSER_URL_OK",
            "SURA_OS_BROWSER_NAV_ASYNC_BEGIN",
            "SURA_OS_BROWSER_NAV_ASYNC_INPUT_OK",
            "SURA_OS_BROWSER_NAV_ASYNC_DONE",
            "SURA_OS_BROWSER_NAV_TCP_BEGIN",
            "SURA_OS_BROWSER_NAV_TCP_INPUT_OK",
            "SURA_OS_BROWSER_NAV_TCP_DONE",
            "SURA_OS_BROWSER_NAV_TLS_BEGIN",
            "SURA_OS_BROWSER_NAV_TLS_DONE",
            "SURA_OS_BROWSER_NAV_FETCH_BEGIN",
            "SURA_OS_BROWSER_NAV_FETCH_INPUT_OK",
            "SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED",
            "SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK",
            "SURA_OS_BROWSER_DOM_BOX_OK",
            "SURA_OS_BROWSER_EXTERNAL_CSS_OK",
            "SURA_OS_BROWSER_CSS_VARIABLES_OK",
            "SURA_OS_BROWSER_CSS_POSITION_OK",
            "SURA_OS_BROWSER_DOM_RENDER_OK",
            "SURA_OS_BROWSER_IMAGE_PNG_OK",
            "SURA_OS_BROWSER_IMAGE_RENDER_OK",
            "SURA_OS_BROWSER_KOREAN_INPUT_OK",
            "SURA_OS_BROWSER_JS_OK",
            "SURA_OS_BROWSER_JS_PAGE_OK",
            "SURA_OS_BROWSER_JS_CLICK_OK",
            "SURA_OS_BROWSER_WASM_OK",
            "SURA_OS_BROWSER_WASM_PAGE_OK",
            "SURA_OS_BROWSER_FORM_SUBMIT_OK",
            "SURA_OS_BROWSER_BACKSPACE_REPEAT_OK",
            "SURA_OS_BROWSER_WHEEL_OK",
            "SURA_OS_DIRECTORY_OK",
            "SURA_OS_STORAGE_WRITE_OK",
            "SURA_OS_SURAFS_EDITOR_SAVE_OK",
            "SURA_OS_SURAFS_FILE_OPEN_OK",
            "SURA_OS_TERMINAL_SCROLL_OK",
            "SURA_OS_CLEAR_OK",
            "kernel: ready"
        )) {
            if (-not $serialText.ToString().Contains($marker)) {
                throw "Sura OS input verification did not observe: $marker"
            }
        }

        # Leave the persisted keyboard preference in Korean mode. The
        # two-boot layout gate reuses this data disk and requires the restore
        # marker from the next boot.
        while ($serialStream.DataAvailable) {
            $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
            if ($read -le 0) { break }
            [void]$serialText.Append(
                [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
            )
        }
        $layoutText = $serialText.ToString()
        if ($layoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH") -gt
            $layoutText.LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN")) {
            [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
                execute = "human-monitor-command"
                arguments = @{ "command-line" = "sendkey alt_r" }
            })
        }
        $layoutPersistDeadline = [DateTime]::UtcNow.AddSeconds(5)
        while ([DateTime]::UtcNow -lt $layoutPersistDeadline -and
               $serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN") -lt
               $serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH")) {
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
        if ($serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_KOREAN") -lt
            $serialText.ToString().LastIndexOf("SURA_OS_INPUT_LAYOUT_ENGLISH")) {
            throw "Final Korean keyboard preference was not processed before shutdown"
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
        # Deliberately corrupt the blocked Calculator process's saved user RIP.
        # The next scheduler wake must isolate its page fault, close only that
        # app, and leave the persistent Terminal process and kernel shell live.
        $faultCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_FAULT")
        )).Count
        $isolatedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_ISOLATED")
        )).Count
        $restartedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_RESTARTED")
        )).Count
        $restartEventCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_RESTART_EVENT_OK")
        )).Count
        $statusCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("kernel: ready")
        )).Count
        $faultBytes = [System.Text.Encoding]::ASCII.GetBytes("faultapp`n")
        $serialStream.Write($faultBytes, 0, $faultBytes.Length)
        $serialStream.Flush()
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_FAULT" ($faultCount + 1) 10)) {
            throw "Graphical user-process fault probe did not reach the page-fault handler"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_ISOLATED" ($isolatedCount + 1) 10)) {
            throw "Graphical user-process fault did not isolate only the Calculator app"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_RESTARTED" ($restartedCount + 1) 10)) {
            throw "Graphical user-process fault did not recreate the Calculator with a new process"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_RESTART_EVENT_OK" ($restartEventCount + 1) 10)) {
            throw "Restarted Calculator did not execute a real event and block again"
        }
        $statusBytes = [System.Text.Encoding]::ASCII.GetBytes("status`n")
        $serialStream.Write($statusBytes, 0, $statusBytes.Length)
        $serialStream.Flush()
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "kernel: ready" ($statusCount + 1) 10)) {
            throw "Kernel shell did not remain responsive after the Calculator page fault"
        }

        # Make the replacement Calculator enter an intentional Ring-3 loop.
        # Move the USB mouse while APIC kernel slices interrupt that loop. The
        # watchdog must observe input progress, terminate only Calculator,
        # reconstruct it again, and leave Terminal responsive.
        $hangStartedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_HANG_STARTED")
        )).Count
        $watchdogCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_WATCHDOG")
        )).Count
        $hangIsolatedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_ISOLATED")
        )).Count
        $hangRestartedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_RESTARTED")
        )).Count
        $hangInputCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_HANG_INPUT_OK")
        )).Count
        $concurrentCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESSES_CONCURRENT_OK")
        )).Count
        $backgroundCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_BACKGROUND_OK")
        )).Count
        $hangRecoveredCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_HANG_RECOVERED")
        )).Count
        $hangBytes = [System.Text.Encoding]::ASCII.GetBytes("hangapp`n")
        $serialStream.Write($hangBytes, 0, $hangBytes.Length)
        $serialStream.Flush()
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_HANG_STARTED" ($hangStartedCount + 1) 10)) {
            throw "Graphical hang diagnostic did not start"
        }
        Send-SuraOsMouseMove $qmpReader $qmpWriter 24 0
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_WATCHDOG" ($watchdogCount + 1) 10)) {
            throw "Graphical watchdog did not terminate the non-yielding Calculator"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_ISOLATED" ($hangIsolatedCount + 1) 10)) {
            throw "Watchdog termination did not isolate only the Calculator app"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_RESTARTED" ($hangRestartedCount + 1) 10)) {
            throw "Watchdog termination did not reconstruct the Calculator"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_HANG_INPUT_OK" ($hangInputCount + 1) 10)) {
            throw "Mouse input did not progress during the non-yielding Calculator loop"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESSES_CONCURRENT_OK" ($concurrentCount + 1) 10)) {
            throw "Calculator and System Information were not runnable concurrently"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_BACKGROUND_OK" ($backgroundCount + 1) 10)) {
            throw "System Information did not complete while Calculator was non-yielding"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_HANG_RECOVERED" ($hangRecoveredCount + 1) 10)) {
            throw "Reconstructed Calculator did not handle an event after watchdog termination"
        }
        $postHangStatusCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("kernel: ready")
        )).Count
        $serialStream.Write($statusBytes, 0, $statusBytes.Length)
        $serialStream.Flush()
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "kernel: ready" ($postHangStatusCount + 1) 10)) {
            throw "Kernel shell did not remain responsive after watchdog recovery"
        }

        # Corrupt the blocked Browser worker's saved RIP. The kernel must
        # isolate only that process, reconstruct it with a new CR3, run the
        # current address through the replacement worker, and keep the shell
        # responsive.
        $browserFaultCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_FAULT")
        )).Count
        $browserIsolatedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_ISOLATED")
        )).Count
        $browserRestartedCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_USER_PROCESS_RESTARTED")
        )).Count
        $browserRecoveryCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("SURA_OS_BROWSER_PROCESS_ISOLATED_OK")
        )).Count
        $browserFaultBytes = [System.Text.Encoding]::ASCII.GetBytes("faultbrowser`n")
        $serialStream.Write($browserFaultBytes, 0, $browserFaultBytes.Length)
        $serialStream.Flush()
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_FAULT" ($browserFaultCount + 1) 10)) {
            throw "Browser worker fault probe did not reach the page-fault handler"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_ISOLATED" ($browserIsolatedCount + 1) 10)) {
            throw "Browser worker fault was not isolated from the desktop"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_USER_PROCESS_RESTARTED" ($browserRestartedCount + 1) 10)) {
            throw "Browser worker was not reconstructed with a new process"
        }
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_BROWSER_PROCESS_ISOLATED_OK" ($browserRecoveryCount + 1) 10)) {
            throw "Replacement Browser worker did not authorize a real request"
        }
        $postBrowserFaultStatusCount = ([regex]::Matches(
            $serialText.ToString(),
            [regex]::Escape("kernel: ready")
        )).Count
        $serialStream.Write($statusBytes, 0, $statusBytes.Length)
        $serialStream.Flush()
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "kernel: ready" ($postBrowserFaultStatusCount + 1) 10)) {
            throw "Kernel shell did not remain responsive after Browser worker recovery"
        }

        # Exercise keyboard-accessible resize, maximize/restore, and minimize.
        # The
        # pointer is still over Terminal's persistent taskbar button, so one
        # click restores the minimized process-backed window.
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey f8" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_WINDOW_RESIZE_OK" 1 10)) {
            throw "F8 did not resize the active window"
        }
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey f10" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_WINDOW_MAXIMIZE_OK" 1 10)) {
            throw "F10 did not maximize the active window"
        }
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey f10" }
        })
        Start-Sleep -Milliseconds 200
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey f9" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_WINDOW_MINIMIZE_OK" 1 10)) {
            throw "F9 did not minimize the active window"
        }
        Send-SuraOsMouseButton $qmpReader $qmpWriter $true
        Send-SuraOsMouseButton $qmpReader $qmpWriter $false
        Start-Sleep -Milliseconds 250

        # Toggle the restored Terminal between windowed and fullscreen mode.
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey f11" }
        })
        if (-not (Wait-SuraOsSerialMarkerCount $serialStream $serialText $qemuProcess "SURA_OS_WINDOW_FULLSCREEN_OK" 1 10)) {
            throw "F11 did not enter the active window's fullscreen state"
        }
        [void](Invoke-SuraOsQmp $qmpReader $qmpWriter @{
            execute = "human-monitor-command"
            arguments = @{ "command-line" = "sendkey f11" }
        })
        Start-Sleep -Milliseconds 250

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
        try {
            while ($serialStream.DataAvailable) {
                $read = $serialStream.Read($serialBuffer, 0, $serialBuffer.Length)
                if ($read -le 0) { break }
                [void]$serialText.Append(
                    [System.Text.Encoding]::ASCII.GetString($serialBuffer, 0, $read)
                )
            }
        }
        catch [System.IO.IOException] {
            # QEMU closes the serial TCP peer as part of a successful ACPI
            # shutdown. All required markers were already collected above.
            break
        }
        Start-Sleep -Milliseconds 20
    }
    $expectedExitCode = 0
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
    $koreanCapture = Get-Item -LiteralPath $KoreanOutput -ErrorAction SilentlyContinue
    $koreanStatus = "not captured"
    if ($null -ne $koreanCapture) {
        $koreanStatus = "$($koreanCapture.FullName), $($koreanCapture.Length) bytes"
    }
    $browserCapture = Get-Item -LiteralPath $BrowserOutput -ErrorAction SilentlyContinue
    $browserStatus = "not captured"
    if ($null -ne $browserCapture) {
        $browserStatus = "$($browserCapture.FullName), $($browserCapture.Length) bytes"
    }
    $browserScrolledCapture = Get-Item -LiteralPath $BrowserScrolledOutput -ErrorAction SilentlyContinue
    $browserScrolledStatus = "not captured"
    if ($null -ne $browserScrolledCapture) {
        $browserScrolledStatus = "$($browserScrolledCapture.FullName), $($browserScrolledCapture.Length) bytes"
    }
    $browserFormCapture = Get-Item -LiteralPath $BrowserFormOutput -ErrorAction SilentlyContinue
    $browserFormStatus = "not captured"
    if ($null -ne $browserFormCapture) {
        $browserFormStatus = "$($browserFormCapture.FullName), $($browserFormCapture.Length) bytes"
    }
    $browserSubmittedCapture = Get-Item -LiteralPath $BrowserSubmittedOutput -ErrorAction SilentlyContinue
    $browserSubmittedStatus = "not captured"
    if ($null -ne $browserSubmittedCapture) {
        $browserSubmittedStatus = "$($browserSubmittedCapture.FullName), $($browserSubmittedCapture.Length) bytes"
    }
    $browserJavascriptCapture = Get-Item -LiteralPath $BrowserJavascriptOutput -ErrorAction SilentlyContinue
    $browserJavascriptStatus = "not captured"
    if ($null -ne $browserJavascriptCapture) {
        $browserJavascriptStatus = "$($browserJavascriptCapture.FullName), $($browserJavascriptCapture.Length) bytes"
    }
    $browserWasmCapture = Get-Item -LiteralPath $BrowserWasmOutput -ErrorAction SilentlyContinue
    $browserWasmStatus = "not captured"
    if ($null -ne $browserWasmCapture) {
        $browserWasmStatus = "$($browserWasmCapture.FullName), $($browserWasmCapture.Length) bytes"
    }
    $browserKoreanCapture = Get-Item -LiteralPath $BrowserKoreanOutput -ErrorAction SilentlyContinue
    $browserKoreanStatus = "not captured"
    if ($null -ne $browserKoreanCapture) {
        $browserKoreanStatus = "$($browserKoreanCapture.FullName), $($browserKoreanCapture.Length) bytes"
    }
    $browserImageErrors = [regex]::Matches(
        $serialText.ToString(),
        'browser image failure: ([0-9]+)'
    ) | ForEach-Object {
        $_.Groups[1].Value
    } | Sort-Object -Unique
    $browserImageErrorStatus = if ($browserImageErrors.Count -gt 0) {
        $browserImageErrors -join ','
    }
    else {
        'none'
    }
    $browserTlsFailureSequences = [regex]::Matches(
        $serialText.ToString(),
        'browser TLS failure sequence: ([0-9]+)'
    ) | ForEach-Object {
        $_.Groups[1].Value
    } | Sort-Object -Unique
    $browserTlsFailureSequenceStatus = if ($browserTlsFailureSequences.Count -gt 0) {
        $browserTlsFailureSequences -join ','
    }
    else {
        'none'
    }
    $browserTlsFailureRecordBytes = [regex]::Matches(
        $serialText.ToString(),
        'browser TLS failure record bytes: ([0-9]+)'
    ) | ForEach-Object {
        $_.Groups[1].Value
    } | Sort-Object -Unique
    $browserTlsFailureRecordBytesStatus = if ($browserTlsFailureRecordBytes.Count -gt 0) {
        $browserTlsFailureRecordBytes -join ','
    }
    else {
        'none'
    }
    "sura_os_screenshot: PASS (desktop=$($capture.FullName), $($capture.Length) bytes; windows=$windowStatus; start=$startStatus; apps=$appsStatus; korean=$koreanStatus; browser=$browserStatus; browser_scrolled=$browserScrolledStatus; browser_form=$browserFormStatus; browser_submitted=$browserSubmittedStatus; browser_javascript=$browserJavascriptStatus; browser_webassembly=$browserWasmStatus; browser_korean=$browserKoreanStatus; browser_image_errors=$browserImageErrorStatus; browser_tls_failure_sequences=$browserTlsFailureSequenceStatus; browser_tls_failure_record_bytes=$browserTlsFailureRecordBytesStatus; input=$inputStatus; data=$dataDiskStatus)"
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
    foreach ($temporaryPath in @($temporaryDisk, $temporaryDataDisk, $temporaryCapture, $temporaryWindowCapture, $temporaryStartCapture, $temporaryAppsCapture, $temporaryKoreanCapture, $temporaryBrowserCapture, $temporaryBrowserScrolledCapture, $temporaryBrowserFormCapture, $temporaryBrowserSubmittedCapture, $temporaryBrowserJavascriptCapture, $temporaryBrowserWasmCapture, $temporaryBrowserKoreanCapture)) {
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
