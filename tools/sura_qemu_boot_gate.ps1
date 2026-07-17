param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/qemu_boot_gate.sura"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [string]$ExpectedEfiText = "Sura QEMU boot gate",
    [string]$ExpectedMarker = "SURA_EXIT_BOOT_SERVICES_OK",
    [int]$ExpectedExitCode = 33,
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

try {
    if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 300) {
        throw "TimeoutSeconds must be 1..300"
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedEfiText) -or
        [string]::IsNullOrWhiteSpace($ExpectedMarker)) {
        throw "ExpectedEfiText and ExpectedMarker must not be empty"
    }
    if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
        throw "Sura engine was not found: $Engine"
    }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "QEMU boot source was not found: $Source"
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
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $process.StartInfo.FileName = $qemuPath
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $qemuArguments = @(
        "-machine", "q35,accel=tcg",
        "-m", "256M",
        "-display", "none",
        "-monitor", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-drive", "if=pflash,format=raw,readonly=on,file=$firmwarePath",
        "-drive", "file=$disk,format=raw,if=ide",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "c"
    )
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
