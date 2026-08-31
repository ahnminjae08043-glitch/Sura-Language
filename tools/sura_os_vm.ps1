param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/SuraLanguage_os_next.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 60,
    [switch]$Interactive,
    [switch]$HeadlessInteractive,
    [switch]$DisableVirtioGpu,
    [switch]$DisableHda,
    [switch]$DisableDoom,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "os/sura_os.sura"
$outputDirectory = Join-Path $root "build/os"
$efi = Join-Path $outputDirectory "SuraOS.efi"
$disk = Join-Path $outputDirectory "SuraOS.img"
$dataDisk = Join-Path $outputDirectory "SuraData.img"
$dataDiskTool = Join-Path $PSScriptRoot "sura_os_data_disk.ps1"
$doomBuild = Join-Path $root "os/doom/build.ps1"
$doomElf = Join-Path $root "os/doom/build/doom.elf"
$doomMarkerText = "SURA_DOOM_ELF_BLOB_V1_20260727!!"
$doomBlobCapacity = 8MB

function Find-UniqueOsDoomMarker {
    param(
        [byte[]]$Bytes,
        [string]$Label
    )
    $text = [System.Text.Encoding]::ASCII.GetString($Bytes)
    $offset = $text.IndexOf($doomMarkerText, [System.StringComparison]::Ordinal)
    if ($offset -lt 0) {
        throw "$Label does not contain the Doom ELF embedding marker"
    }
    if ($text.IndexOf($doomMarkerText, $offset + 1, [System.StringComparison]::Ordinal) -ge 0) {
        throw "$Label contains more than one Doom ELF embedding marker"
    }
    return $offset
}

function Add-OsDoomElf {
    param(
        [string]$Path,
        [byte[]]$ElfBytes,
        [string]$Label
    )
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $offset = Find-UniqueOsDoomMarker $bytes $Label
    if ($offset + $doomBlobCapacity -gt $bytes.Length) {
        throw "$Label does not contain the complete $doomBlobCapacity-byte Doom storage region"
    }
    if ($ElfBytes.Length -gt $doomBlobCapacity) {
        throw "Doom ELF is $($ElfBytes.Length) bytes; embedding capacity is $doomBlobCapacity"
    }
    [Array]::Copy($ElfBytes, 0, $bytes, $offset, $ElfBytes.Length)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
    return $offset
}

function Add-OsDoomApplication {
    if (-not (Test-Path -LiteralPath $doomBuild -PathType Leaf)) {
        throw "Doom build script was not found: $doomBuild"
    }
    Write-Host "Building the Sura desktop Doom application..."
    & $doomBuild
    if (-not $? -or -not (Test-Path -LiteralPath $doomElf -PathType Leaf)) {
        throw "Doom ELF build failed"
    }
    $elfBytes = [System.IO.File]::ReadAllBytes($doomElf)
    if ($elfBytes.Length -lt 64 -or
        $elfBytes[0] -ne 0x7f -or $elfBytes[1] -ne 0x45 -or
        $elfBytes[2] -ne 0x4c -or $elfBytes[3] -ne 0x46 -or
        $elfBytes[4] -ne 2 -or
        [BitConverter]::ToUInt16($elfBytes, 16) -ne 2 -or
        [BitConverter]::ToUInt16($elfBytes, 18) -ne 62) {
        throw "Doom build did not produce a static x86-64 ELF executable"
    }
    $elfEntry = [BitConverter]::ToUInt64($elfBytes, 24)
    if ((($elfEntry -shr 39) -band 0x1ff) -ne 2) {
        throw ("Doom ELF entry 0x{0:x} is not linked in PML4 slot 2" -f $elfEntry)
    }
    $efiOffset = Add-OsDoomElf $efi $elfBytes "SuraOS.efi"
    $diskOffset = Add-OsDoomElf $disk $elfBytes "SuraOS.img"
    Write-Host ("Doom embedded: ELF={0} bytes, EFI offset={1}, disk offset={2}" -f
        $elfBytes.Length, $efiOffset, $diskOffset)
}

function Resolve-OsQemu {
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

function Resolve-OsFirmware {
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

function ConvertTo-OsNativeArgument {
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

function Get-OsFreeTcpPort {
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

function Read-OsSerialAvailable {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [int]$WaitMilliseconds = 300
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($WaitMilliseconds)
    $buffer = New-Object byte[] 4096
    $builder = New-Object System.Text.StringBuilder
    do {
        try {
            while ($Stream.DataAvailable) {
                $read = $Stream.Read($buffer, 0, $buffer.Length)
                if ($read -le 0) { break }
                [void]$builder.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            }
        }
        catch {
            break
        }
        if ([DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 20
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    $text = $builder.ToString()
    if ($text.Length -gt 0) {
        $ansiPattern = ([string][char]27) + '\[[0-?]*[ -/]*[@-~]'
        $clean = [regex]::Replace($text, $ansiPattern, "")
        Write-Host -NoNewline $clean
        return $clean
    }
    return ""
}

function Read-OsSerialTail {
    param([System.Net.Sockets.NetworkStream]$Stream)
    $buffer = New-Object byte[] 4096
    $builder = New-Object System.Text.StringBuilder
    $Stream.ReadTimeout = 1000
    while ($true) {
        try {
            $read = $Stream.Read($buffer, 0, $buffer.Length)
        }
        catch {
            break
        }
        if ($read -le 0) { break }
        [void]$builder.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
    }
    $text = $builder.ToString()
    if ($text.Length -gt 0) {
        $ansiPattern = ([string][char]27) + '\[[0-?]*[ -/]*[@-~]'
        $clean = [regex]::Replace($text, $ansiPattern, "")
        Write-Host -NoNewline $clean
        return $clean
    }
    return ""
}

try {
    if ($Interactive -and $CompileOnly) {
        throw "Interactive and CompileOnly cannot be used together"
    }
    if ($HeadlessInteractive -and -not $Interactive) {
        throw "HeadlessInteractive requires Interactive"
    }
    if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
        throw "Sura engine was not found: $Engine"
    }
    $Engine = (Resolve-Path -LiteralPath $Engine).Path
    Write-Host "sura_os_engine: $Engine"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Sura OS source was not found: $source"
    }
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    & $dataDiskTool -Path $dataDisk
    if (-not $?) {
        throw "Sura OS data disk creation failed"
    }

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $compileOutput = & $Engine --target uefi-x86_64 --out $efi --disk-image $disk $source 2>&1
    $compileExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    if ($compileExitCode -ne 0) {
        $compileText = $compileOutput -join "`n"
        if ($compileText -match "unknown freestanding intrinsic or function 'cpu\.rdrand'") {
            $currentOsEngine = Join-Path $root "build/SuraLanguage_os_next.exe"
            if (Test-Path -LiteralPath $currentOsEngine -PathType Leaf) {
                throw "Sura OS compile failed because this engine predates the cpu.rdrand OS intrinsic. Run again with -Engine `"$currentOsEngine`".`n$compileText"
            }
        }
        throw "Sura OS compile failed:`n$compileText"
    }
    if (-not $DisableDoom) {
        Add-OsDoomApplication
    }

    if ($Interactive) {
        $qemuPath = Resolve-OsQemu $Qemu
        $firmwarePath = Resolve-OsFirmware $Firmware $qemuPath
        $interactiveDisk = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_os_interactive_" + [guid]::NewGuid().ToString("N") + ".img")
        $interactiveDataDisk = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_os_interactive_data_" + [guid]::NewGuid().ToString("N") + ".img")
        Copy-Item -LiteralPath $disk -Destination $interactiveDisk -Force
        Copy-Item -LiteralPath $dataDisk -Destination $interactiveDataDisk -Force
        $qemuProcess = $null
        $serialClient = $null
        $serialStream = $null
        try {
            Write-Host "Sura OS interactive shell"
            Write-Host "Type help for commands. Type shutdown or reboot to close QEMU."
            $serialPort = Get-OsFreeTcpPort
            $displayBackend = "gtk"
            if ($HeadlessInteractive) { $displayBackend = "none" }
            $qemuArguments = @(
                "-machine", "q35,accel=tcg",
                "-cpu", "max",
                "-m", "256M",
                "-display", $displayBackend,
                "-monitor", "none",
                "-serial", "tcp:127.0.0.1:$serialPort,server=on,wait=off",
                "-no-reboot",
                "-drive", "if=pflash,format=raw,readonly=on,file=$firmwarePath",
                "-drive", "file=$interactiveDisk,format=raw,if=ide,index=0",
                "-drive", "file=$interactiveDataDisk,format=raw,if=ide,index=1",
                "-netdev", "user,id=suranet",
                "-device", "virtio-net-pci,netdev=suranet,disable-modern=on,mac=52:54:00:12:34:56",
                "-device", "qemu-xhci,id=sura-xhci",
                "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
                "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-boot", "c"
            )
            if (-not $DisableVirtioGpu) {
                $qemuArguments += @(
                    "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=1280,yres=800"
                )
            }
            if (-not $DisableHda) {
                $qemuArguments += @(
                    "-audiodev", "none,id=sura-audio",
                    "-device", "ich9-intel-hda,id=sura-hda,msi=off",
                    "-device", "hda-output,audiodev=sura-audio"
                )
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
                        ConvertTo-OsNativeArgument ([string]$_)
                    }) -join " ")
            }
            if (-not $qemuProcess.Start()) { throw "Interactive QEMU did not start" }
            $qemuStdoutTask = $qemuProcess.StandardOutput.ReadToEndAsync()
            $qemuStderrTask = $qemuProcess.StandardError.ReadToEndAsync()

            $connectTimeoutSeconds = [Math]::Min(
                60,
                [Math]::Max(15, $TimeoutSeconds)
            )
            $connectDeadline = [DateTime]::UtcNow.AddSeconds($connectTimeoutSeconds)
            while ([DateTime]::UtcNow -lt $connectDeadline -and $null -eq $serialClient) {
                if ($qemuProcess.HasExited) { break }
                $candidateClient = New-Object System.Net.Sockets.TcpClient
                try {
                    $candidateClient.Connect("127.0.0.1", $serialPort)
                    $serialClient = $candidateClient
                }
                catch {
                    $candidateClient.Dispose()
                    Start-Sleep -Milliseconds 100
                }
            }
            if ($null -eq $serialClient) {
                $exitDetail = ""
                if ($qemuProcess.HasExited) {
                    $qemuDiagnostics = $qemuStderrTask.GetAwaiter().GetResult()
                    $exitDetail = " QEMU exited with code $($qemuProcess.ExitCode).`n$qemuDiagnostics"
                }
                throw "Could not connect to the QEMU serial bridge within $connectTimeoutSeconds seconds.$exitDetail"
            }
            $serialStream = $serialClient.GetStream()
            $bootText = ""
            $bootDeadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
            while ([DateTime]::UtcNow -lt $bootDeadline -and
                   -not $bootText.Contains("Sura OS shell ready") -and
                   -not $qemuProcess.HasExited) {
                $bootText += Read-OsSerialAvailable $serialStream 250
            }
            if (-not $bootText.Contains("Sura OS shell ready")) {
                $exitDetail = ""
                if ($qemuProcess.HasExited) {
                    $exitDetail = " (QEMU exit=$($qemuProcess.ExitCode))"
                }
                throw "Sura OS serial shell did not become ready within $TimeoutSeconds seconds$exitDetail`n$bootText"
            }

            while (-not $qemuProcess.HasExited) {
                $command = Read-Host
                if ($null -eq $command) {
                    throw "Interactive input was closed before a shutdown or reboot request"
                }
                $commandBytes = [System.Text.Encoding]::ASCII.GetBytes($command + "`n")
                $serialStream.Write($commandBytes, 0, $commandBytes.Length)
                $serialStream.Flush()
                Start-Sleep -Milliseconds 100
                [void](Read-OsSerialAvailable $serialStream 400)
                $normalizedCommand = $command.Trim().ToLowerInvariant()
                if ($normalizedCommand -eq "shutdown" -or $normalizedCommand -eq "reboot") {
                    break
                }
            }
            if (-not $qemuProcess.WaitForExit(10000)) {
                throw "Interactive QEMU did not exit after the requested power action"
            }
            [void](Read-OsSerialTail $serialStream)
            $qemuExitCode = $qemuProcess.ExitCode
            if ($qemuExitCode -ne 0 -and $qemuExitCode -ne 33 -and $qemuExitCode -ne 35 -and $qemuExitCode -ne 37) {
                $qemuDiagnostics = $qemuStderrTask.GetAwaiter().GetResult()
                throw "Interactive QEMU closed with unexpected exit code $qemuExitCode`n$qemuDiagnostics"
            }
            Copy-Item -LiteralPath $interactiveDataDisk -Destination $dataDisk -Force
            "sura_os_vm: DATA SAVED ($dataDisk)"
            "sura_os_vm: INTERACTIVE CLOSED (exit=$qemuExitCode)"
        }
        finally {
            if ($null -ne $serialStream) {
                $serialStream.Dispose()
            }
            if ($null -ne $serialClient) {
                $serialClient.Dispose()
            }
            if ($null -ne $qemuProcess) {
                if (-not $qemuProcess.HasExited) {
                    $qemuProcess.Kill()
                    $qemuProcess.WaitForExit()
                }
                $qemuProcess.Dispose()
            }
            if (Test-Path -LiteralPath $interactiveDisk -PathType Leaf) {
                $resolvedInteractiveDisk = [System.IO.Path]::GetFullPath($interactiveDisk)
                $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
                if ($resolvedInteractiveDisk.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
                    (Split-Path -Leaf $resolvedInteractiveDisk).StartsWith("sura_os_interactive_")) {
                    Remove-Item -LiteralPath $resolvedInteractiveDisk -Force
                }
            }
            if (Test-Path -LiteralPath $interactiveDataDisk -PathType Leaf) {
                $resolvedInteractiveDataDisk = [System.IO.Path]::GetFullPath($interactiveDataDisk)
                $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
                if ($resolvedInteractiveDataDisk.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
                    (Split-Path -Leaf $resolvedInteractiveDataDisk).StartsWith("sura_os_interactive_data_")) {
                    Remove-Item -LiteralPath $resolvedInteractiveDataDisk -Force
                }
            }
        }
        return
    }

    $graphicsMarker = "SURA_OS_VIRTIO_GPU_READY"
    $audioMarker = "SURA_OS_HDA_AUDIO_READY"
    $osQemuArguments = @(
        "-device", "qemu-xhci,id=sura-xhci",
        "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
        "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0"
    )
    if ($DisableHda) {
        $audioMarker = "SURA_OS_HDA_AUDIO_UNAVAILABLE"
    }
    else {
        $osQemuArguments += @(
            "-audiodev", "none,id=sura-audio",
            "-device", "ich9-intel-hda,id=sura-hda,msi=off",
            "-device", "hda-output,audiodev=sura-audio"
        )
    }
    if ($DisableVirtioGpu) {
        $graphicsMarker = "SURA_OS_VIRTIO_GPU_FALLBACK"
    }
    else {
        $osQemuArguments = @(
            "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=1280,yres=800"
        ) + $osQemuArguments
    }
    $osExpectedMarkers = @(
        "SURA_OS_ACPI_POWER_READY", "SURA_OS_ACPI_POWER_OFF_ARMED",
        $graphicsMarker, $audioMarker,
        "SURA_OS_DESKTOP_OK", "SURA_OS_WINDOW_READY", "SURA_OS_RTC_OK",
        "SURA_OS_PS2_READY", "SURA_OS_XHCI_INPUT_READY", "SURA_OS_INPUT_EVENT_OK",
        "SURA_OS_KEYBOARD_OK", "SURA_OS_MOUSE_OK", "SURA_OS_STORAGE_READY",
        "SURA_OS_STORAGE_AHCI_READY",
        "SURA_OS_STORAGE_READ_OK", "SURA_OS_SURAFS_READY", "SURA_OS_SETTINGS_READY",
        "SURA_OS_DESKTOP_STATE_READY", "SURA_OS_DHCP_OK", "SURA_OS_NETWORK_READY",
        "SURA_OS_ARP_OK", "SURA_OS_UDP_OK", "SURA_OS_DNS_OK", "SURA_OS_TCP_OK",
        "SURA_OS_HTTP_OK", "SURA_OS_BROWSER_APP_OK", "SURA_OS_BROWSER_CSS_OK",
        "SURA_OS_BROWSER_DOM_BOX_OK",
        "SURA_OS_CALCULATOR_RING3_READY", "SURA_OS_EDITOR_RING3_READY",
        "SURA_OS_FILES_RING3_READY", "SURA_OS_TERMINAL_RING3_READY",
        "SURA_OS_SYSTEM_RING3_READY", "SURA_OS_USER_SCHEDULER_READY",
        "SURA_OS_TERMINAL_RING3_OK", "SURA_OS_TERMINAL_CR3_OK", "dns example.com: ",
        "commands: help status mem about clear shutdown reboot", "kernel: ready",
        "free physical pages: ", "Sura OS: freestanding x86-64 kernel running in QEMU",
        "SURA_OS_CLEAR_OK"
    )

    & (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
        -Engine $Engine `
        -Source $source `
        -DataDisk $dataDisk `
        -EnableNetwork `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura OS virtual machine" `
        -ExpectedMarker "SURA_OS_SHUTDOWN" `
        -ExpectedExitCode 0 `
        -SerialInputLines @("help", "status", "mem", "about", "clear", "shutdown") `
        -SerialInputDelayMilliseconds 9500 `
        -SerialInputIntervalMilliseconds 1000 `
        -AdditionalExpectedSerialMarkers $osExpectedMarkers `
        -AdditionalQemuArguments $osQemuArguments `
        -QmpSendKey "shift 1000" `
        -QmpMouseDeltaX 40 `
        -QmpMouseDeltaY 20 `
        -QmpInputDelayMilliseconds 7500 `
        -CompileOnly:$CompileOnly
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $efiLength = (Get-Item -LiteralPath $efi).Length
    $diskLength = (Get-Item -LiteralPath $disk).Length
    if ($CompileOnly) {
        "sura_os_vm: COMPILE PASS (efi=$efiLength, disk=$diskLength bytes)"
    }
    else {
        "sura_os_vm: PASS (efi=$efiLength, disk=$diskLength bytes)"
    }
}
catch {
    Write-Error $_
    exit 1
}
