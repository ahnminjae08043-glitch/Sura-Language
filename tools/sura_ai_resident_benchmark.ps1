param(
    [string]$Engine = "",
    [string]$Python = "",
    [string]$JsonOut = "",
    [string]$MarkdownOut = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Engine)) {
    $Engine = Join-Path $root "SuraLanguage.exe"
}
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$benchmarkPath = Join-Path $root "benchmarks/ai_resident_cuda_benchmark.sura"
if ([string]::IsNullOrWhiteSpace($JsonOut)) {
    $JsonOut = Join-Path $root "artifacts/ai_resident_cuda_benchmark.json"
}
if ([string]::IsNullOrWhiteSpace($MarkdownOut)) {
    $MarkdownOut = Join-Path $root "artifacts/ai_resident_cuda_benchmark.md"
}

$pythonPath = $null
if (-not [string]::IsNullOrWhiteSpace($Python)) {
    $pythonPath = (Resolve-Path -LiteralPath $Python).Path
} else {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) { $pythonPath = $pythonCommand.Source }
}

# A standalone Sura installation does not bundle NVIDIA's large cuBLAS
# redistributables.  For the same-hardware comparison, reuse the exact cuBLAS
# DLL already shipped with the selected PyTorch runtime when the caller did not
# explicitly select another one.  Sura still verifies and reports whether it
# actually used cuBLAS, and the measured region excludes this discovery step.
$previousCublasLibrary = $env:SURA_CUBLAS_LIBRARY
$benchmarkCublasLibrary = $previousCublasLibrary
if ([string]::IsNullOrWhiteSpace($benchmarkCublasLibrary) -and $pythonPath) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        # Keep Python string literals single-quoted inside a PowerShell
        # double-quoted argument. Windows PowerShell 5 otherwise strips the
        # nested double quotes while constructing the native command line.
        $pythonProbe = "import pathlib, torch; p = pathlib.Path(torch.__file__).parent / 'lib'; c = sorted(p.glob('cublas64_*.dll'), reverse=True); print(c[0] if c else '')"
        $probe = & $pythonPath -c $pythonProbe 2>$null
        if ($LASTEXITCODE -eq 0) {
            $candidate = [string]($probe | Select-Object -First 1)
            if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
                $benchmarkCublasLibrary = (Resolve-Path -LiteralPath $candidate).Path
                $env:SURA_CUBLAS_LIBRARY = $benchmarkCublasLibrary
            }
        }
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$inputPath = Join-Path $tempRoot (
    "sura-ai-resident-bench-" + [guid]::NewGuid().ToString("N") + ".safetensors")
$previousInput = $env:SURA_RESIDENT_BENCH_INPUTS
try {
    $env:SURA_RESIDENT_BENCH_INPUTS = $inputPath
    $suraOutput = & $enginePath $benchmarkPath 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "Sura resident CUDA benchmark failed:`n$($suraOutput -join [Environment]::NewLine)"
    }
    $suraLine = $suraOutput |
        Where-Object { $_.TrimStart().StartsWith("{") } |
        Select-Object -Last 1
    if (-not $suraLine) { throw "Sura resident benchmark did not emit JSON" }
    $sura = $suraLine | ConvertFrom-Json

    $torch = [ordered]@{ status = "unavailable"; reason = "python executable not found" }
    if ($pythonPath -and (Test-Path -LiteralPath $inputPath)) {
        $torchScript = Join-Path $root "benchmarks/ai_resident_cuda_benchmark_torch.py"
        $suraDeviceIndex = if ($sura.status -eq "ok") {
            [int]$sura.device_index
        } else { 0 }
        $torchArgs = @(
            $torchScript, $inputPath,
            "--warmup", "3", "--runs", "10", "--learning-rate", "0.0001",
            "--device-index", [string]$suraDeviceIndex)
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $torchOutput = & $pythonPath @torchArgs 2>&1 |
                ForEach-Object { "$_" }
            $torchExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($torchExitCode -eq 0) {
            $torchLine = $torchOutput |
                Where-Object { $_.TrimStart().StartsWith("{") } |
                Select-Object -Last 1
            if ($torchLine) { $torch = $torchLine | ConvertFrom-Json }
        } else {
            $torch = [ordered]@{
                status = "error"
                reason = ($torchOutput -join [Environment]::NewLine)
            }
        }
    }

    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem
    $gpu = Get-CimInstance Win32_VideoController |
        Where-Object { $_.Name -match "NVIDIA" } |
        Select-Object -First 1
    $suraOk = $sura.status -eq "ok"
    $torchOk = $torch.status -eq "ok"
    $sameGpuDevice = $null
    if ($suraOk -and $torchOk) {
        $sameGpuDevice = [string]$sura.device -eq [string]$torch.device_name
    }
    $forwardTransferFree = $suraOk -and [bool]$sura.forward.resident_transfer_free
    $trainingTransferFree = $suraOk -and [bool]$sura.training.resident_tensor_transfer_free
    $cublasUsed = $suraOk -and
        [int64]$sura.forward.stats_before_read.cublas_matmul_launches -gt 0
    $checksumAbsError = $null
    $checksumTolerance = $null
    $sameInitialChecksum = $null
    if ($suraOk -and $torchOk) {
        $checksumAbsError = [Math]::Abs(
            [double]$sura.forward.initial_checksum - [double]$torch.initial_checksum)
        $checksumTolerance = [Math]::Max(
            0.1, [Math]::Abs([double]$torch.initial_checksum) * 0.00001)
        $sameInitialChecksum = $checksumAbsError -le $checksumTolerance
    }

    $report = [ordered]@{
        schema = "sura.ai-resident-benchmark-report.v1"
        generated_at_utc = [DateTime]::UtcNow.ToString("o")
        fairness = [ordered]@{
            same_host = $true
            same_gpu_device = $sameGpuDevice
            same_inputs_file = [bool](Test-Path -LiteralPath $inputPath)
            dtype = "float32"
            matrix_size = 256
            warmup = 3
            runs = 10
            process_startup_excluded = $true
            setup_and_initial_upload_excluded = $true
            synchronization_inside_each_sample = $true
            workload = "eager public APIs; relu(matmul) forward and SGD training step"
            claim_scope = "resident float32 kernels currently implemented by Sura"
        }
        validation = [ordered]@{
            sura_available = $suraOk
            forward_zero_h2d_d2h_before_read = $forwardTransferFree
                       training_only_optimizer_status_d2h_before_read = $trainingTransferFree
                       cublas_used_for_forward_matmul = $cublasUsed
            same_initial_checksum = $sameInitialChecksum
            initial_checksum_tolerance = $checksumTolerance
        }
        hardware = [ordered]@{
            cpu = [string]$cpu.Name
            logical_processors = [int]$cpu.NumberOfLogicalProcessors
            memory_bytes = [int64]$os.TotalVisibleMemorySize * 1024
            gpu = [string]$gpu.Name
            gpu_driver = [string]$gpu.DriverVersion
        }
        runtime = [ordered]@{
            engine = [IO.Path]::GetFileName($enginePath)
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $enginePath).Hash.ToLowerInvariant()
            pytorch_python = [string]$pythonPath
            cublas_library = [string]$benchmarkCublasLibrary
        }
        sura = $sura
        pytorch = $torch
    }
    if ($suraOk -and $torchOk) {
        $report.fairness["initial_checksum_absolute_error"] = $checksumAbsError
        $report.fairness["forward_sura_over_pytorch"] =
            [double]$sura.forward.median_ms / [double]$torch.forward.median_ms
        $report.fairness["training_sura_over_pytorch"] =
            [double]$sura.training.median_step_ms / [double]$torch.training.median_step_ms
    }

    $jsonParent = Split-Path -Parent $JsonOut
    $markdownParent = Split-Path -Parent $MarkdownOut
    if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
    if ($markdownParent) { New-Item -ItemType Directory -Force -Path $markdownParent | Out-Null }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText(
        $JsonOut, ($report | ConvertTo-Json -Depth 12), $utf8)

    $suraForward = if ($suraOk) {
        "$([Math]::Round([double]$sura.forward.median_ms, 4)) ms"
    } else { "unavailable: $($sura.reason)" }
    $suraTraining = if ($suraOk) {
        "$([Math]::Round([double]$sura.training.median_step_ms, 4)) ms/step"
    } else { "unavailable" }
    $torchForward = if ($torchOk) {
        "$([Math]::Round([double]$torch.forward.median_ms, 4)) ms"
    } else { "unavailable: $($torch.reason)" }
    $torchTraining = if ($torchOk) {
        "$([Math]::Round([double]$torch.training.median_step_ms, 4)) ms/step"
    } else { "unavailable" }
    $forwardRatio = if ($suraOk -and $torchOk) {
        "$([Math]::Round([double]$report.fairness.forward_sura_over_pytorch, 3))x"
    } else { "unavailable" }
    $trainingRatio = if ($suraOk -and $torchOk) {
        "$([Math]::Round([double]$report.fairness.training_sura_over_pytorch, 3))x"
    } else { "unavailable" }
    $forwardCounters = if ($suraOk) {
        "H2D=$($sura.forward.stats_before_read.h2d_bytes), D2H=$($sura.forward.stats_before_read.d2h_bytes), D2D=$($sura.forward.stats_before_read.d2d_bytes), matmul=$($sura.forward.stats_before_read.matmul_launches), cublas=$($sura.forward.stats_before_read.cublas_matmul_launches), reference=$($sura.forward.stats_before_read.reference_matmul_launches), relu=$($sura.forward.stats_before_read.relu_launches)"
    } else { "unavailable" }
    $trainingCounters = if ($suraOk) {
        "H2D=$($sura.training.stats_before_read.h2d_bytes), D2H=$($sura.training.stats_before_read.d2h_bytes), D2D=$($sura.training.stats_before_read.d2d_bytes), kernels=$($sura.training.stats_before_read.kernel_launches)"
    } else { "unavailable" }
    $markdown = @"
# Sura GPU-Resident AI Benchmark

- Workload: 256x256 float32 eager execution on $($gpu.Name)
- Sura matmul backend: $($sura.matmul_backend)
- Sura forward median: $suraForward
- PyTorch forward median: $torchForward
- Sura training median: $suraTraining
- PyTorch training median: $torchTraining
- Forward time ratio (Sura/PyTorch, lower is better): $forwardRatio
- Training time ratio (Sura/PyTorch, lower is better): $trainingRatio
- Sura forward counters before scalar read: $forwardCounters
- Sura training counters before scalar read: $trainingCounters
- Forward transfer-free validation: $forwardTransferFree
- Training bulk-tensor-transfer-free validation: $trainingTransferFree

The setup/upload phase and process startup are excluded. Every timed sample is
explicitly synchronized. Both runtimes use the same Safetensors input state.
The training step is `zero_grad + matmul + relu + mse + backward + SGD`.
Transactional SGD reads one shared four-byte finite-status word per measured
step; this control read is reported separately and no Tensor payload moves.
The result measures only the currently implemented resident float32 path; it is
not evidence of full PyTorch feature or performance parity.
"@
    [IO.File]::WriteAllText($MarkdownOut, $markdown, $utf8)

    if ($suraOk -and (-not $forwardTransferFree -or -not $trainingTransferFree)) {
        throw "resident CUDA validation failed: timed regions performed an unexpected tensor transfer"
    }
    if ($suraOk -and -not [string]::IsNullOrWhiteSpace($benchmarkCublasLibrary) -and
        -not $cublasUsed) {
        throw "cuBLAS validation failed: a benchmark library was selected but Sura did not dispatch matmul through it"
    }
    Write-Host "sura_ai_resident_benchmark: PASS"
    Write-Host "json: $([IO.Path]::GetFullPath($JsonOut))"
    Write-Host "markdown: $([IO.Path]::GetFullPath($MarkdownOut))"
}
finally {
    $env:SURA_RESIDENT_BENCH_INPUTS = $previousInput
    $env:SURA_CUBLAS_LIBRARY = $previousCublasLibrary
    $resolved = [IO.Path]::GetFullPath($inputPath)
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith("sura-ai-resident-bench-") -and
        (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Force
    }
}
