param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/SuraLanguage_os_next.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [ValidateRange(30, 300)]
    [int]$TimeoutSeconds = 180,
    [ValidateSet("tcg", "whpx")]
    [string]$Acceleration = "tcg",
    [string]$ScreenshotPath = "",
    [switch]$Interactive,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/doom_qemu_gate.sura"
$doomDirectory = Join-Path $root "os/doom"
$doomBuild = Join-Path $doomDirectory "build.ps1"
$doomElf = Join-Path $doomDirectory "build/doom.elf"
$outputDirectory = Join-Path $root "build/doom"
$efi = Join-Path $outputDirectory "SuraDoom.efi"
$disk = Join-Path $outputDirectory "SuraDoom.img"
$serialLog = Join-Path $outputDirectory "SuraDoom.serial.log"
$markerText = "SURA_DOOM_ELF_BLOB_V1_20260727!!"
$blobCapacity = 8MB

function Resolve-DoomQemu {
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

function Resolve-DoomFirmware {
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

function ConvertTo-DoomNativeArgument {
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

function Get-DoomFreeTcpPort {
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

function Connect-DoomQmp {
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

function Invoke-DoomQmp {
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

function Find-UniqueDoomMarker {
    param(
        [byte[]]$Bytes,
        [string]$Marker,
        [string]$Label
    )
    # ASCII decoding preserves one character per byte, so the string index is
    # also the exact byte offset even for arbitrary bytes elsewhere in a PE.
    $text = [System.Text.Encoding]::ASCII.GetString($Bytes)
    $offset = $text.IndexOf($Marker, [System.StringComparison]::Ordinal)
    if ($offset -lt 0) {
        throw "$Label does not contain the Doom ELF embedding marker"
    }
    if ($text.IndexOf($Marker, $offset + 1, [System.StringComparison]::Ordinal) -ge 0) {
        throw "$Label contains more than one Doom ELF embedding marker"
    }
    return $offset
}

function Add-DoomElfToImage {
    param(
        [string]$Path,
        [byte[]]$ElfBytes,
        [string]$Label
    )
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $offset = Find-UniqueDoomMarker $bytes $markerText $Label
    if ($offset + $blobCapacity -gt $bytes.Length) {
        throw "$Label does not have the complete $blobCapacity-byte Doom storage region"
    }
    if ($ElfBytes.Length -gt $blobCapacity) {
        throw "Doom ELF is $($ElfBytes.Length) bytes; embedding capacity is $blobCapacity"
    }
    [Array]::Copy($ElfBytes, 0, $bytes, $offset, $ElfBytes.Length)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
    return $offset
}

if ($Interactive -and $CompileOnly) {
    throw "Interactive and CompileOnly cannot be used together"
}
if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
    throw "Sura OS engine was not found: $Engine"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Doom kernel source was not found: $source"
}
if (-not (Test-Path -LiteralPath $doomBuild -PathType Leaf)) {
    throw "Doom build script was not found: $doomBuild"
}

$Engine = (Resolve-Path -LiteralPath $Engine).Path
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

Write-Host "Building freestanding Doom ELF..."
& $doomBuild
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $doomElf -PathType Leaf)) {
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
$elfPml4Index = ($elfEntry -shr 39) -band 0x1ff
if ($elfPml4Index -ne 2) {
    throw ("Doom ELF entry 0x{0:x} is not linked in PML4 slot 2" -f $elfEntry)
}

Write-Host "Compiling Sura Doom kernel..."
$compileOutput = & $Engine --target uefi-x86_64 --out $efi --disk-image $disk $source 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "Sura Doom kernel compile failed:`n$($compileOutput -join "`n")"
}
if (-not (Test-Path -LiteralPath $efi -PathType Leaf) -or
    -not (Test-Path -LiteralPath $disk -PathType Leaf)) {
    throw "Sura Doom kernel compile did not produce both EFI and disk images"
}

$efiOffset = Add-DoomElfToImage $efi $elfBytes "SuraDoom.efi"
$diskOffset = Add-DoomElfToImage $disk $elfBytes "SuraDoom.img"
$patchedEfi = [System.IO.File]::ReadAllBytes($efi)
if ($patchedEfi[$efiOffset] -ne 0x7f -or
    $patchedEfi[$efiOffset + 1] -ne 0x45 -or
    $patchedEfi[$efiOffset + 2] -ne 0x4c -or
    $patchedEfi[$efiOffset + 3] -ne 0x46) {
    throw "Patched EFI image does not contain the ELF magic at its storage offset"
}

Write-Host ("SuraDoom built: ELF={0} bytes, EFI={1} bytes, disk={2} bytes" -f
    $elfBytes.Length,
    (Get-Item -LiteralPath $efi).Length,
    (Get-Item -LiteralPath $disk).Length)
if ($CompileOnly) {
    "sura_doom_qemu_gate: COMPILE PASS (entry=0x$($elfEntry.ToString('x')), efi_offset=$efiOffset, disk_offset=$diskOffset)"
    return
}

$qemuPath = Resolve-DoomQemu $Qemu
$firmwarePath = Resolve-DoomFirmware $Firmware $qemuPath
$runtimeDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_doom_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null
$runtimeDisk = Join-Path $runtimeDirectory "SuraDoom.img"
$runtimeSerialLog = Join-Path $runtimeDirectory "SuraDoom.serial.log"
$runtimeScreenshot = Join-Path $runtimeDirectory "SuraDoom.ppm"
Copy-Item -LiteralPath $disk -Destination $runtimeDisk -Force
$commonArguments = @(
    "-machine", "q35",
    "-m", "256M",
    "-vga", "std",
    "-monitor", "none",
    "-no-reboot",
    "-drive", "if=pflash,format=raw,readonly=on,file=$firmwarePath",
    # QEMU's Windows file backend is not reliable with every Unicode path.
    # Run from an ASCII-safe temporary copy and retain artifacts afterward.
    "-drive", "file=$runtimeDisk,format=raw,if=ide,index=0",
    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    "-boot", "c",
    "-name", "Sura OS Doom"
)
if ($Acceleration -eq "tcg") {
    $commonArguments = @("-accel", "tcg,tb-size=256", "-cpu", "max") + $commonArguments
}
else {
    $commonArguments = @("-accel", "whpx", "-cpu", "max") + $commonArguments
}

if ($Interactive) {
    Write-Host ""
    Write-Host "Sura OS Doom is starting. Click the QEMU window to capture input."
    Write-Host "Arrow keys move, Ctrl fires, Space uses, Esc opens the menu, F12 exits."
    $interactiveArguments = $commonArguments + @(
        "-display", "gtk,zoom-to-fit=on",
        "-serial", "stdio"
    )
    try {
        & $qemuPath @interactiveArguments
        $interactiveExit = $LASTEXITCODE
        if ($interactiveExit -ne 0 -and $interactiveExit -ne 33) {
            throw "Interactive SuraDoom closed with unexpected QEMU exit code $interactiveExit"
        }
        "sura_doom_qemu_gate: INTERACTIVE CLOSED (exit=$interactiveExit)"
    }
    finally {
        if (Test-Path -LiteralPath $runtimeDirectory -PathType Container) {
            $resolvedRuntime = [System.IO.Path]::GetFullPath($runtimeDirectory)
            $resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
            if ($resolvedRuntime.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
                (Split-Path -Leaf $resolvedRuntime).StartsWith("sura_doom_")) {
                Remove-Item -LiteralPath $resolvedRuntime -Recurse -Force
            }
        }
    }
    return
}

if ([string]::IsNullOrWhiteSpace($ScreenshotPath)) {
    $ScreenshotPath = Join-Path $outputDirectory "SuraDoom.ppm"
}
$ScreenshotPath = [System.IO.Path]::GetFullPath($ScreenshotPath)
$screenshotParent = Split-Path -Parent $ScreenshotPath
if (-not (Test-Path -LiteralPath $screenshotParent -PathType Container)) {
    throw "Screenshot parent directory was not found: $screenshotParent"
}
foreach ($generated in @($serialLog, $ScreenshotPath)) {
    if (Test-Path -LiteralPath $generated -PathType Leaf) {
        Remove-Item -LiteralPath $generated -Force
    }
}

$qmpPort = Get-DoomFreeTcpPort
$smokeArguments = $commonArguments + @(
    "-display", "none",
    "-serial", "file:$runtimeSerialLog",
    "-qmp", "tcp:127.0.0.1:$qmpPort,server=on,wait=off"
)

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
$process.StartInfo.FileName = $qemuPath
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.CreateNoWindow = $true
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true
if ($process.StartInfo.PSObject.Properties.Name -contains "ArgumentList") {
    foreach ($argument in $smokeArguments) {
        $process.StartInfo.ArgumentList.Add($argument)
    }
}
else {
    $process.StartInfo.Arguments =
        (($smokeArguments | ForEach-Object {
            ConvertTo-DoomNativeArgument ([string]$_)
        }) -join " ")
}

$qmpClient = $null
$qmpReader = $null
$qmpWriter = $null
try {
    if (-not $process.Start()) { throw "QEMU did not start" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)

    try {
        $qmpClient = Connect-DoomQmp $qmpPort ([datetime]::UtcNow.AddSeconds(10))
    }
    catch {
        if ($process.HasExited) {
            $earlyStdout = $stdoutTask.GetAwaiter().GetResult()
            $earlyStderr = $stderrTask.GetAwaiter().GetResult()
            throw "QEMU exited before QMP became ready (exit=$($process.ExitCode)):`n$earlyStdout`n$earlyStderr"
        }
        throw
    }
    $qmpStream = $qmpClient.GetStream()
    $qmpStream.ReadTimeout = 10000
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
    [void](Invoke-DoomQmp $qmpReader $qmpWriter @{ execute = "qmp_capabilities" })

    $serialText = ""
    while ([datetime]::UtcNow -lt $deadline -and
           -not $process.HasExited -and
           -not $serialText.Contains("SURA_DOOM_FRAME_OK")) {
        if (Test-Path -LiteralPath $runtimeSerialLog -PathType Leaf) {
            $serialText = Get-Content -LiteralPath $runtimeSerialLog -Raw -ErrorAction SilentlyContinue
            if ($null -eq $serialText) { $serialText = "" }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $serialText.Contains("SURA_DOOM_FRAME_OK")) {
        throw "Doom did not render a frame before the timeout:`n$serialText"
    }

    [void](Invoke-DoomQmp $qmpReader $qmpWriter @{
        execute = "screendump"
        arguments = @{
            filename = $runtimeScreenshot
            format = "ppm"
        }
    })
    [void](Invoke-DoomQmp $qmpReader $qmpWriter @{
        execute = "human-monitor-command"
        arguments = @{ "command-line" = "sendkey f12" }
    })

    while ([datetime]::UtcNow -lt $deadline -and -not $process.HasExited) {
        Start-Sleep -Milliseconds 50
    }
    if (-not $process.HasExited) {
        throw "Doom did not exit after the injected F12 key"
    }

    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if (Test-Path -LiteralPath $runtimeSerialLog -PathType Leaf) {
        $serialText = Get-Content -LiteralPath $runtimeSerialLog -Raw
    }
    $expectedMarkers = @(
        "SURA_DOOM_KERNEL_READY",
        "SURA_DOOM_ELF_LOADED",
        "SURA_DOOM_INPUT_READY",
        "SURA_DOOM_RING3_READY",
        "SURA_DOOM_MAIN",
        "SURA_DOOM_DG_INIT",
        "SURA_DOOM_FRAME_OK",
        "SURA_DOOM_INPUT_OK",
        "SURA_DOOM_PLAYABLE"
    )
    foreach ($marker in $expectedMarkers) {
        if (-not $serialText.Contains($marker)) {
            throw "Doom serial output is missing '$marker':`n$serialText`n$stdout`n$stderr"
        }
    }
    if ($process.ExitCode -ne 33) {
        throw "Doom QEMU smoke test exited with code $($process.ExitCode):`n$serialText`n$stdout`n$stderr"
    }
    if (-not (Test-Path -LiteralPath $runtimeScreenshot -PathType Leaf) -or
        (Get-Item -LiteralPath $runtimeScreenshot).Length -lt 1024) {
        throw "Doom smoke test did not produce a valid framebuffer screenshot"
    }

    Copy-Item -LiteralPath $runtimeSerialLog -Destination $serialLog -Force
    Copy-Item -LiteralPath $runtimeScreenshot -Destination $ScreenshotPath -Force
    "sura_doom_qemu_gate: PASS (exit=$($process.ExitCode), screenshot=$ScreenshotPath)"
}
finally {
    if ($null -ne $qmpWriter) { $qmpWriter.Dispose() }
    if ($null -ne $qmpReader) { $qmpReader.Dispose() }
    if ($null -ne $qmpClient) { $qmpClient.Dispose() }
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
    $process.Dispose()
    if (Test-Path -LiteralPath $runtimeSerialLog -PathType Leaf) {
        Copy-Item -LiteralPath $runtimeSerialLog -Destination $serialLog -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $runtimeDirectory -PathType Container) {
        $resolvedRuntime = [System.IO.Path]::GetFullPath($runtimeDirectory)
        $resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRuntime.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedRuntime).StartsWith("sura_doom_")) {
            Remove-Item -LiteralPath $resolvedRuntime -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
