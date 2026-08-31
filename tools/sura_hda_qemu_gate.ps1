param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 45,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bootGate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"
$source = Join-Path $root "examples/os/hda_qemu_gate.sura"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_hda_gate_" + [guid]::NewGuid().ToString("N"))
$wav = Join-Path $temp "sura-hda-output.wav"

function Test-SuraHdaWave {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "QEMU did not create the HDA WAV capture"
    }
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 44) { throw "HDA WAV capture is too short" }
        $riff = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
        [void]$reader.ReadUInt32()
        $wave = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
        if ($riff -ne "RIFF" -or $wave -ne "WAVE") {
            throw "HDA capture is not a RIFF/WAVE file"
        }

        $format = 0
        $channels = 0
        $sampleRate = 0
        $bitsPerSample = 0
        [byte[]]$data = $null
        while ($stream.Position + 8 -le $stream.Length) {
            $chunk = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
            $size = $reader.ReadUInt32()
            if ($stream.Position + $size -gt $stream.Length) {
                throw "HDA WAV chunk exceeds the file"
            }
            if ($chunk -eq "fmt ") {
                if ($size -lt 16) { throw "HDA WAV fmt chunk is too short" }
                $format = $reader.ReadUInt16()
                $channels = $reader.ReadUInt16()
                $sampleRate = $reader.ReadUInt32()
                [void]$reader.ReadUInt32()
                [void]$reader.ReadUInt16()
                $bitsPerSample = $reader.ReadUInt16()
                if ($size -gt 16) { [void]$reader.ReadBytes([int]($size - 16)) }
            }
            elseif ($chunk -eq "data") {
                # isa-debug-exit terminates QEMU before the WAV backend can
                # rewrite its provisional RIFF/data sizes. In that case the
                # PCM payload is still complete and occupies the file tail.
                if ($size -eq 0 -and $stream.Position -lt $stream.Length) {
                    $data = $reader.ReadBytes([int]($stream.Length - $stream.Position))
                    break
                }
                $data = $reader.ReadBytes([int]$size)
            }
            else {
                [void]$reader.ReadBytes([int]$size)
            }
            if (($size -band 1) -ne 0 -and $stream.Position -lt $stream.Length) {
                [void]$reader.ReadByte()
            }
        }
        if ($format -ne 1 -or $channels -ne 2 -or
            $sampleRate -ne 48000 -or $bitsPerSample -ne 16) {
            throw "Unexpected HDA WAV format (format=$format channels=$channels rate=$sampleRate bits=$bitsPerSample)"
        }
        if ($null -eq $data -or $data.Length -lt 131072 -or
            ($data.Length % 4) -ne 0) {
            $captured = if ($null -eq $data) { 0 } else { $data.Length }
            throw "HDA WAV capture does not contain enough stereo PCM data (bytes=$captured)"
        }
        $minimum = 32767
        $maximum = -32768
        for ($offset = 0; $offset + 1 -lt $data.Length; $offset += 2) {
            $sample = [System.BitConverter]::ToInt16($data, $offset)
            if ($sample -lt $minimum) { $minimum = $sample }
            if ($sample -gt $maximum) { $maximum = $sample }
        }
        if ($minimum -gt -1000 -or $maximum -lt 1000) {
            throw "HDA WAV capture does not contain the expected bipolar waveform (min=$minimum max=$maximum)"
        }
        return [pscustomobject]@{
            Bytes = $data.Length
            Minimum = $minimum
            Maximum = $maximum
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

try {
    New-Item -ItemType Directory -Path $temp | Out-Null
    $qemuWavPath = $wav.Replace('\', '/')
    & $bootGate `
        -Engine $Engine `
        -Source $source `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura Intel HDA executed gate" `
        -ExpectedMarker "SURA_HDA_EXECUTED_OK" `
        -AdditionalExpectedSerialMarkers @(
            "SURA_HDA_PCI_OK",
            "SURA_HDA_RESET_OK",
            "SURA_HDA_CODEC_OK",
            "SURA_HDA_VENDOR_OK",
            "SURA_HDA_ROOT_NODES_OK",
            "SURA_HDA_AUDIO_FUNCTION_GROUP_OK",
            "SURA_HDA_PCM_CAPS_OK",
            "SURA_HDA_OUTPUT_WIDGETS_OK",
            "SURA_HDA_PCM_BDL_OK",
            "SURA_HDA_STREAM_RUNNING_OK",
            "SURA_HDA_LPIB_ADVANCED_OK"
        ) `
        -AdditionalQemuArguments @(
            "-audiodev", "wav,id=sura-audio,path=$qemuWavPath,out.frequency=48000",
            "-device", "ich9-intel-hda,id=sura-hda,msi=off",
            "-device", "hda-output,audiodev=sura-audio"
        ) `
        -CompileOnly:$CompileOnly
    if (-not $?) {
        throw "Sura Intel HDA executed QEMU gate failed"
    }

    if ($CompileOnly) {
        "sura_hda_qemu_gate: COMPILE PASS"
    }
    else {
        $wave = Test-SuraHdaWave $wav
        "sura_hda_qemu_gate: PASS (PCI/MMIO codec verbs, output widgets, BDL DMA, LPIB=$($wave.Bytes) captured PCM bytes, 48-kHz s16 stereo, sample min=$($wave.Minimum) max=$($wave.Maximum))"
    }
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolved = [System.IO.Path]::GetFullPath($temp)
        $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_hda_gate_")) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
