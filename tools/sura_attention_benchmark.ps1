param(
    [string]$Engine = "",
    [string]$Python = "",
    [int]$BatchSize = 1,
    [int]$Heads = 4,
    [int]$SequenceLength = 64,
    [int]$HeadDim = 32,
    [int]$Warmup = 3,
    [int]$Runs = 10,
    [int]$CpuThreads = 1,
    [double]$ValidationTolerance = 0.001,
    [int]$MaxPreflightGpuUtilization = 5,
    [switch]$AllowBusyGpu,
    [string]$JsonOut = "",
    [string]$MarkdownOut = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Engine)) {
    $Engine = Join-Path $root "SuraLanguage.exe"
}
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$benchmarkPath = Join-Path $root "benchmarks/causal_attention_benchmark.sura"
$torchScript = Join-Path $root "benchmarks/causal_attention_benchmark_torch.py"
if ([string]::IsNullOrWhiteSpace($JsonOut)) {
    $JsonOut = Join-Path $root "artifacts/attention_benchmark.json"
}
if ([string]::IsNullOrWhiteSpace($MarkdownOut)) {
    $MarkdownOut = Join-Path $root "artifacts/attention_benchmark.md"
}

if ($BatchSize -lt 1 -or $Heads -lt 1 -or $SequenceLength -lt 1 -or
    $HeadDim -lt 1 -or $Runs -lt 1 -or $CpuThreads -lt 1) {
    throw "BatchSize, Heads, SequenceLength, HeadDim, Runs, and CpuThreads must be positive"
}
if ($Warmup -lt 0) { throw "Warmup must be non-negative" }
if ($ValidationTolerance -le 0) { throw "ValidationTolerance must be positive" }
if ($MaxPreflightGpuUtilization -lt 0 -or $MaxPreflightGpuUtilization -gt 100) {
    throw "MaxPreflightGpuUtilization must be between 0 and 100"
}

$scoreCount = [int64]$BatchSize * [int64]$Heads *
    [int64]$SequenceLength * [int64]$SequenceLength
if ($scoreCount -gt 5000000000) {
    throw "B*H*T*T exceeds Sura's configurable attention-score ceiling of 5,000,000,000"
}

$pythonPath = $null
if (-not [string]::IsNullOrWhiteSpace($Python)) {
    $pythonPath = (Resolve-Path -LiteralPath $Python).Path
} else {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) { $pythonPath = $pythonCommand.Source }
}

function Get-FingerprintErrors {
    param($Left, $Right)
    if ($null -eq $Left -or $null -eq $Right) { return $null }
    return [ordered]@{
        loss = [Math]::Abs([double]$Left.loss - [double]$Right.loss)
        q_grad_sum = [Math]::Abs([double]$Left.q_grad_sum - [double]$Right.q_grad_sum)
        q_grad_l1 = [Math]::Abs([double]$Left.q_grad_l1 - [double]$Right.q_grad_l1)
        k_grad_sum = [Math]::Abs([double]$Left.k_grad_sum - [double]$Right.k_grad_sum)
        k_grad_l1 = [Math]::Abs([double]$Left.k_grad_l1 - [double]$Right.k_grad_l1)
        v_grad_sum = [Math]::Abs([double]$Left.v_grad_sum - [double]$Right.v_grad_sum)
        v_grad_l1 = [Math]::Abs([double]$Left.v_grad_l1 - [double]$Right.v_grad_l1)
    }
}

function Test-FingerprintErrors {
    param($Errors, [double]$Tolerance)
    if ($null -eq $Errors) { return $null }
    $values = if ($Errors -is [Collections.IDictionary]) {
        $Errors.Values
    } else {
        $Errors.PSObject.Properties.Value
    }
    foreach ($value in $values) {
        if ([double]$value -gt $Tolerance) { return $false }
    }
    return $true
}

function Format-MarkdownCell {
    param([object]$Value)
    if ($null -eq $Value) { return "-" }
    return (([string]$Value) -replace "`r?`n", " " -replace "\|", "\|")
}

function Format-BenchmarkRow {
    param([string]$RuntimeName, $Entry)
    if ($null -eq $Entry) {
        return "| $RuntimeName | - | SKIP: no result | - | - |"
    }
    $status = [string]$Entry.status
    $device = Format-MarkdownCell $Entry.device
    if ($status -eq "ok") {
        $median = [Math]::Round([double]$Entry.median_ms, 4)
        $p95 = [Math]::Round([double]$Entry.p95_ms, 4)
        return "| $RuntimeName | $device | ok | $median | $p95 |"
    }
    $reason = Format-MarkdownCell $Entry.reason
    if ([string]::IsNullOrWhiteSpace($reason) -or $reason -eq "-") {
        $reason = $status
    }
    return "| $RuntimeName | $device | SKIP: $reason | - | - |"
}

function New-UnavailableEntry {
    param([string]$Device, [string]$Reason)
    return [pscustomobject]@{
        status = "unavailable"
        device = $Device
        reason = $Reason
    }
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$inputPath = Join-Path $tempRoot (
    "sura-attention-bench-" + [guid]::NewGuid().ToString("N") + ".safetensors")
$environmentNames = @(
    "SURA_ATTENTION_BENCH_INPUTS",
    "SURA_ATTENTION_BENCH_BATCH",
    "SURA_ATTENTION_BENCH_HEADS",
    "SURA_ATTENTION_BENCH_SEQUENCE",
    "SURA_ATTENTION_BENCH_HEAD_DIM",
    "SURA_ATTENTION_BENCH_WARMUP",
    "SURA_ATTENTION_BENCH_RUNS"
)
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
    $env:SURA_ATTENTION_BENCH_INPUTS = $inputPath
    $env:SURA_ATTENTION_BENCH_BATCH = [string]$BatchSize
    $env:SURA_ATTENTION_BENCH_HEADS = [string]$Heads
    $env:SURA_ATTENTION_BENCH_SEQUENCE = [string]$SequenceLength
    $env:SURA_ATTENTION_BENCH_HEAD_DIM = [string]$HeadDim
    $env:SURA_ATTENTION_BENCH_WARMUP = [string]$Warmup
    $env:SURA_ATTENTION_BENCH_RUNS = [string]$Runs

    $selectedDeviceIndex = 0
    $requestedDevice = [Environment]::GetEnvironmentVariable("SURA_CUDA_DEVICE", "Process")
    if (-not [string]::IsNullOrWhiteSpace($requestedDevice)) {
        $parsedDevice = 0
        if (-not [int]::TryParse($requestedDevice, [ref]$parsedDevice) -or $parsedDevice -lt 0) {
            throw "SURA_CUDA_DEVICE must be a non-negative integer for benchmark preflight"
        }
        $selectedDeviceIndex = $parsedDevice
    }
    $gpuPreflightSamples = @()
    $gpuPreflightUtilization = $null
    $gpuPreflightPeakUtilization = $null
    $nvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($nvidiaSmi) {
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            for ($sampleIndex = 0; $sampleIndex -lt 7; $sampleIndex++) {
                $utilizationLines = & $nvidiaSmi.Source `
                    "--query-gpu=index,utilization.gpu" `
                    "--format=csv,noheader,nounits" 2>$null
                if ($LASTEXITCODE -eq 0) {
                    foreach ($line in $utilizationLines) {
                        $parts = ([string]$line).Split(",")
                        if ($parts.Count -lt 2) { continue }
                        $indexValue = 0
                        $utilizationValue = 0
                        if ([int]::TryParse($parts[0].Trim(), [ref]$indexValue) -and
                            [int]::TryParse($parts[1].Trim(), [ref]$utilizationValue) -and
                            $indexValue -eq $selectedDeviceIndex) {
                            $gpuPreflightSamples += $utilizationValue
                            break
                        }
                    }
                }
                if ($sampleIndex -lt 6) { Start-Sleep -Seconds 1 }
            }
            if ($gpuPreflightSamples.Count -gt 0) {
                $sortedUtilization = @($gpuPreflightSamples | Sort-Object)
                $gpuPreflightUtilization = $sortedUtilization[
                    [int][Math]::Floor($sortedUtilization.Count / 2)]
                $gpuPreflightPeakUtilization = $sortedUtilization[-1]
            }
        } finally {
            $ErrorActionPreference = $oldPreference
        }
    }
    $gpuWasBusy = $null -ne $gpuPreflightUtilization -and
        $gpuPreflightUtilization -gt $MaxPreflightGpuUtilization
    $performanceValid = $null -ne $gpuPreflightUtilization -and -not $gpuWasBusy
    if ($gpuWasBusy -and -not $AllowBusyGpu) {
        throw "GPU $selectedDeviceIndex median utilization is $gpuPreflightUtilization% across seven one-second samples ($($gpuPreflightSamples -join ', ')%), above the public benchmark limit of $MaxPreflightGpuUtilization%. Close GPU workloads or pass -AllowBusyGpu for a non-public diagnostic run."
    }

    $suraOutput = & $enginePath $benchmarkPath 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "Sura causal-attention benchmark failed:`n$($suraOutput -join [Environment]::NewLine)"
    }
    $suraLine = $suraOutput |
        Where-Object { $_.TrimStart().StartsWith("{") } |
        Select-Object -Last 1
    if (-not $suraLine) { throw "Sura causal-attention benchmark did not emit JSON" }
    $sura = $suraLine | ConvertFrom-Json
    if ($sura.status -ne "ok" -or $sura.cpu.status -ne "ok") {
        throw "Sura CPU causal-attention reference did not complete successfully"
    }
    if ([string]$sura.precision -ne "auto") {
        throw "Sura causal-attention benchmark did not report precision=auto"
    }

    $torch = [pscustomobject]@{
        status = "unavailable"
        reason = "PyTorch comparison skipped: python executable not found"
    }
    if ($pythonPath -and (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        $deviceIndex = if ($sura.cuda.status -eq "ok") {
            [int]$sura.cuda.device_index
        } else { 0 }
        $torchArgs = @(
            $torchScript, $inputPath,
            "--batch", [string]$BatchSize,
            "--heads", [string]$Heads,
            "--sequence", [string]$SequenceLength,
            "--head-dim", [string]$HeadDim,
            "--warmup", [string]$Warmup,
            "--runs", [string]$Runs,
            "--cpu-threads", [string]$CpuThreads,
            "--device-index", [string]$deviceIndex
        )
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
            if ($torchLine) {
                $torch = $torchLine | ConvertFrom-Json
            } else {
                $torch = [pscustomobject]@{
                    status = "error"
                    reason = "PyTorch helper emitted no JSON"
                }
            }
        } else {
            $torch = [pscustomobject]@{
                status = "error"
                reason = ($torchOutput -join [Environment]::NewLine)
            }
        }
    }

    if ($torch.status -ne "ok") {
        $torchReason = [string]$torch.reason
        $torchStatus = [string]$torch.status
        $torch = [pscustomobject]@{
            status = $torchStatus
            reason = $torchReason
            cpu = New-UnavailableEntry "cpu" $torchReason
            cuda = New-UnavailableEntry "cuda" $torchReason
        }
    }

    $torchCpu = if ($torch.status -eq "ok") {
        $torch.cpu
    } else {
        New-UnavailableEntry "cpu" ([string]$torch.reason)
    }
    $torchCuda = if ($torch.status -eq "ok") {
        $torch.cuda
    } else {
        New-UnavailableEntry "cuda" ([string]$torch.reason)
    }

    $cpuName = [Environment]::GetEnvironmentVariable("PROCESSOR_IDENTIFIER")
    $logicalProcessors = [Environment]::ProcessorCount
    $memoryBytes = $null
    $cimGpuName = $null
    $cimGpuDriver = $null
    if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
        try {
            $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
            $os = Get-CimInstance Win32_OperatingSystem
            $gpu = Get-CimInstance Win32_VideoController |
                Where-Object { $_.Name -match "NVIDIA" } |
                Select-Object -First 1
            if ($cpu) {
                $cpuName = [string]$cpu.Name
                $logicalProcessors = [int]$cpu.NumberOfLogicalProcessors
            }
            if ($os) { $memoryBytes = [int64]$os.TotalVisibleMemorySize * 1024 }
            if ($gpu) {
                $cimGpuName = [string]$gpu.Name
                $cimGpuDriver = [string]$gpu.DriverVersion
            }
        } catch {
            # Hardware metadata is informative; benchmark results remain valid.
        }
    }

    $gpuName = $cimGpuName
    if ($sura.cuda.status -eq "ok") { $gpuName = [string]$sura.cuda.device }
    $sameGpuDevice = $null
    if ($sura.cuda.status -eq "ok" -and $torchCuda.status -eq "ok") {
        $sameGpuDevice = [string]$sura.cuda.device -eq [string]$torchCuda.device_name
    }

    $cpuRatio = $null
    if ($torchCpu.status -eq "ok" -and [double]$torchCpu.median_ms -gt 0) {
        $cpuRatio = [double]$sura.cpu.median_ms / [double]$torchCpu.median_ms
    }
    $cudaRatio = $null
    if ($sura.cuda.status -eq "ok" -and $torchCuda.status -eq "ok" -and
        [double]$torchCuda.median_ms -gt 0) {
        $cudaRatio = [double]$sura.cuda.median_ms / [double]$torchCuda.median_ms
    }

    $inputChecksumError = $null
    $suraTorchCpuErrors = $null
    if ($torch.status -eq "ok") {
        $inputChecksumError = [Math]::Abs(
            [double]$sura.input_checksum - [double]$torch.input_checksum)
        $suraTorchCpuErrors = Get-FingerprintErrors $sura.cpu.fingerprint $torchCpu.fingerprint
    }
    $suraCpuCudaErrors = $null
    if ($sura.cuda.status -eq "ok") {
        $suraCpuCudaErrors = Get-FingerprintErrors $sura.cpu.fingerprint $sura.cuda.fingerprint
    }
    $suraCpuCudaValid = Test-FingerprintErrors $suraCpuCudaErrors $ValidationTolerance
    $suraTorchCpuValid = Test-FingerprintErrors $suraTorchCpuErrors $ValidationTolerance
    $packedWorkspaceBytes = [int64]$BatchSize * [int64]$Heads *
        [int64]$SequenceLength * ([int64]$SequenceLength + 1L) / 2L * 4L
    $expectedFusedAttention = $sura.cuda.status -eq "ok" -and
        [bool]$sura.cuda.fused_enabled -and
        $SequenceLength -ge [int]$sura.cuda.parallel_min_sequence
    $expectedPackedAttention = $sura.cuda.status -eq "ok" -and
        -not $expectedFusedAttention -and
        [bool]$sura.cuda.parallel_enabled -and
        $SequenceLength -ge [int]$sura.cuda.parallel_min_sequence -and
        $packedWorkspaceBytes -le [int64]$sura.cuda.workspace_limit_bytes
    $attentionCounterContractValid = $null
    if ($sura.cuda.status -eq "ok") {
        $totalPerRun = [double]$sura.cuda.attention_launches_per_run
        $referencePerRun = [double]$sura.cuda.reference_attention_launches_per_run
        $warpPerRun = [double]$sura.cuda.warp_attention_launches_per_run
        $parallelPerRun = [double]$sura.cuda.parallel_attention_launches_per_run
        $fusedPerRun = [double]$sura.cuda.fused_attention_launches_per_run
        $fastForwardPerRun = [double]$sura.cuda.fast_attention_forward_launches_per_run
        $attentionCounterContractValid =
            [Math]::Abs($totalPerRun - ($referencePerRun + $warpPerRun + $parallelPerRun + $fusedPerRun)) -lt 0.000001 -and
            $(if ($expectedFusedAttention) {
                $totalPerRun -eq 3 -and $referencePerRun -eq 0 -and
                    $warpPerRun -eq 1 -and $parallelPerRun -eq 0 -and
                    $fusedPerRun -eq 2 -and $fastForwardPerRun -eq 1
            } elseif ($expectedPackedAttention) {
                $totalPerRun -eq 6 -and $referencePerRun -eq 0 -and
                    $warpPerRun -eq 1 -and $parallelPerRun -eq 5 -and
                    $fusedPerRun -eq 0 -and $fastForwardPerRun -eq 0
            } else {
                $totalPerRun -eq 2 -and $referencePerRun -eq 2 -and
                    $warpPerRun -eq 0 -and $parallelPerRun -eq 0 -and
                    $fusedPerRun -eq 0 -and $fastForwardPerRun -eq 0
            })
    }

    $report = [ordered]@{
        schema = "sura.attention-benchmark-report.v1"
        generated_at_utc = [DateTime]::UtcNow.ToString("o")
        fairness = [ordered]@{
            same_host = $true
            same_gpu_device = $sameGpuDevice
            same_inputs_file = [bool](Test-Path -LiteralPath $inputPath -PathType Leaf)
            input_checksum_absolute_error = $inputChecksumError
            dtype = "float32"
            sura_attention_precision = "auto"
            sura_cpu_compute = "double-over-float32-storage"
            sura_cuda_compute = "float32"
            pytorch_compute = "float32"
            shape = [ordered]@{
                batch = $BatchSize
                heads = $Heads
                sequence = $SequenceLength
                head_dim = $HeadDim
            }
            causal = $true
            scale = "1/sqrt(head_dim)"
            loss_reduction = "sum"
            layout = "B,H,T,D"
            transpose_included = $false
            projections_included = $false
            warmup = $Warmup
            runs = $Runs
            gpu_preflight_device_index = $selectedDeviceIndex
            gpu_preflight_utilization_percent = $gpuPreflightUtilization
            gpu_preflight_peak_utilization_percent = $gpuPreflightPeakUtilization
            gpu_preflight_samples_percent = @($gpuPreflightSamples)
            gpu_preflight_statistic = "median of seven one-second nvidia-smi utilization samples"
            max_preflight_gpu_utilization_percent = $MaxPreflightGpuUtilization
            allow_busy_gpu = [bool]$AllowBusyGpu
            performance_valid = $performanceValid
            process_startup_excluded = $true
            input_creation_and_upload_excluded = $true
            gradient_zeroing_excluded = $true
            synchronization_inside_each_cuda_sample = $true
            sura_cpu_threads = 1
            pytorch_cpu_threads = $(if ($torch.status -eq "ok") {
                [int]$torch.cpu_threads
            } else { $null })
            workload = "eager rank-4 causal_attention + scalar sum + backward"
            claim_scope = "current Sura f32 warp online-softmax forward and deterministic no-O(T^2)-workspace fused recomputation backward versus PyTorch SDPA; not an end-to-end Transformer, shared-memory tiled/Tensor Core FlashAttention, or framework-wide parity claim"
            cpu_sura_over_pytorch = $cpuRatio
            cuda_sura_over_pytorch = $cudaRatio
        }
        validation = [ordered]@{
            sura_cpu_reference_completed = $true
            performance_valid = $performanceValid
            gpu_preflight_available = $null -ne $gpuPreflightUtilization
            gpu_preflight_busy = $gpuWasBusy
            sura_cuda_available = $sura.cuda.status -eq "ok"
            sura_cuda_resident_transfer_free = $(if ($sura.cuda.status -eq "ok") {
                [bool]$sura.cuda.resident_transfer_free
            } else { $null })
            sura_cuda_attention_launches_actual = $(if ($sura.cuda.status -eq "ok") {
                [int64]$sura.cuda.stats_before_read.attention_launches
            } else { $null })
            sura_cuda_attention_launches_per_run = $(if ($sura.cuda.status -eq "ok") {
                [double]$sura.cuda.attention_launches_per_run
            } else { $null })
            sura_cuda_reference_attention_launches = $(if ($sura.cuda.status -eq "ok") {
                [int64]$sura.cuda.reference_attention_launches
            } else { $null })
            sura_cuda_warp_attention_launches = $(if ($sura.cuda.status -eq "ok") {
                [int64]$sura.cuda.warp_attention_launches
            } else { $null })
            sura_cuda_parallel_attention_launches = $(if ($sura.cuda.status -eq "ok") {
                [int64]$sura.cuda.parallel_attention_launches
            } else { $null })
            sura_cuda_reference_attention_launches_per_run = $(if ($sura.cuda.status -eq "ok") {
                [double]$sura.cuda.reference_attention_launches_per_run
            } else { $null })
            sura_cuda_warp_attention_launches_per_run = $(if ($sura.cuda.status -eq "ok") {
                [double]$sura.cuda.warp_attention_launches_per_run
            } else { $null })
             sura_cuda_parallel_attention_launches_per_run = $(if ($sura.cuda.status -eq "ok") {
                 [double]$sura.cuda.parallel_attention_launches_per_run
             } else { $null })
             sura_cuda_fused_attention_launches = $(if ($sura.cuda.status -eq "ok") {
                 [int64]$sura.cuda.fused_attention_launches
             } else { $null })
             sura_cuda_fused_attention_launches_per_run = $(if ($sura.cuda.status -eq "ok") {
                 [double]$sura.cuda.fused_attention_launches_per_run
             } else { $null })
             sura_cuda_fast_attention_forward_launches = $(if ($sura.cuda.status -eq "ok") {
                 [int64]$sura.cuda.fast_attention_forward_launches
             } else { $null })
             sura_cuda_fast_attention_forward_launches_per_run = $(if ($sura.cuda.status -eq "ok") {
                 [double]$sura.cuda.fast_attention_forward_launches_per_run
             } else { $null })
            sura_cuda_expected_parallel_path = $expectedFusedAttention -or $expectedPackedAttention
            sura_cuda_expected_fused_path = $expectedFusedAttention
            sura_cuda_expected_legacy_packed_path = $expectedPackedAttention
            sura_cuda_expected_reference_path = -not ($expectedFusedAttention -or $expectedPackedAttention)
            sura_cuda_attention_counter_contract_valid = $attentionCounterContractValid
            pytorch_status = [string]$torch.status
            fingerprint_absolute_tolerance = $ValidationTolerance
            sura_cpu_cuda_fingerprint_within_tolerance = $suraCpuCudaValid
            sura_pytorch_cpu_fingerprint_within_tolerance = $suraTorchCpuValid
            sura_cpu_cuda_fingerprint_absolute_errors = $suraCpuCudaErrors
            sura_pytorch_cpu_fingerprint_absolute_errors = $suraTorchCpuErrors
        }
        hardware = [ordered]@{
            cpu = [string]$cpuName
            logical_processors = [int]$logicalProcessors
            memory_bytes = $memoryBytes
            gpu = [string]$gpuName
            gpu_driver = [string]$cimGpuDriver
        }
        runtime = [ordered]@{
            engine = [IO.Path]::GetFileName($enginePath)
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $enginePath).Hash.ToLowerInvariant()
            python = [string]$pythonPath
        }
        sura = $sura
        pytorch = $torch
    }

    $jsonParent = Split-Path -Parent $JsonOut
    $markdownParent = Split-Path -Parent $MarkdownOut
    if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
    if ($markdownParent) { New-Item -ItemType Directory -Force -Path $markdownParent | Out-Null }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText(
        $JsonOut, ($report | ConvertTo-Json -Depth 14), $utf8)

    $suraCpuRow = Format-BenchmarkRow "Sura" $sura.cpu
    $suraCudaRow = Format-BenchmarkRow "Sura" $sura.cuda
    $torchCpuRow = Format-BenchmarkRow "PyTorch" $torchCpu
    $torchCudaRow = Format-BenchmarkRow "PyTorch" $torchCuda
    $cpuRatioText = if ($null -ne $cpuRatio) {
        "$([Math]::Round($cpuRatio, 3))x"
    } else { "unavailable" }
    $cudaRatioText = if ($null -ne $cudaRatio) {
        "$([Math]::Round($cudaRatio, 3))x"
    } else { "unavailable" }
    $transferText = if ($sura.cuda.status -eq "ok") {
        "H2D=$($sura.cuda.stats_before_read.h2d_bytes), D2H=$($sura.cuda.stats_before_read.d2h_bytes), total attention launches=$($sura.cuda.stats_before_read.attention_launches) (reference=$($sura.cuda.reference_attention_launches), warp=$($sura.cuda.warp_attention_launches), legacy-packed-backward=$($sura.cuda.parallel_attention_launches), fused-recompute-backward=$($sura.cuda.fused_attention_launches), fast-forward=$($sura.cuda.fast_attention_forward_launches)); per run=$($sura.cuda.attention_launches_per_run) (reference=$($sura.cuda.reference_attention_launches_per_run), warp=$($sura.cuda.warp_attention_launches_per_run), legacy-packed-backward=$($sura.cuda.parallel_attention_launches_per_run), fused-recompute-backward=$($sura.cuda.fused_attention_launches_per_run), fast-forward=$($sura.cuda.fast_attention_forward_launches_per_run))"
    } else {
        "SKIP: $($sura.cuda.reason)"
    }
    $torchStatusText = if ($torch.status -eq "ok") {
        "ok (PyTorch $($torch.torch_version))"
    } else {
        "SKIP: $($torch.reason)"
    }
    $markdown = @"
# Sura Causal Attention Benchmark

- Shape: B=$BatchSize, H=$Heads, T=$SequenceLength, D=$HeadDim
- Workload: float32 causal attention forward + scalar sum + backward
- Sura attention precision policy: precision: "auto"
- Layout/scope: direct B,H,T,D input; head split/merge, transpose, and projections excluded
- Warmup/repetitions: $Warmup / $Runs
- GPU preflight utilization: median $gpuPreflightUtilization%, peak $gpuPreflightPeakUtilization% across seven one-second samples ``$($gpuPreflightSamples -join ', ')`` (public median limit $MaxPreflightGpuUtilization%; performance valid: $performanceValid)
- PyTorch comparison: $torchStatusText

| Runtime | Device | Status | Median ms | P95 ms |
|---|---|---:|---:|---:|
$suraCpuRow
$suraCudaRow
$torchCpuRow
$torchCudaRow

- CPU time ratio (Sura/PyTorch, lower is better): $cpuRatioText
- CUDA time ratio (Sura/PyTorch, lower is better): $cudaRatioText
- Sura CUDA counters before observation: $transferText
- Same CUDA device: $sameGpuDevice
- Shared-input checksum absolute error: $inputChecksumError
- Sura CPU/CUDA fingerprint within tolerance: $suraCpuCudaValid
- Sura/PyTorch CPU fingerprint within tolerance: $suraTorchCpuValid
- Fingerprint absolute tolerance: $ValidationTolerance

Process startup, input construction/upload, gradient clearing, and final host
observation are excluded. CUDA samples synchronize at their boundaries. Sura
and PyTorch consume the same Safetensors q/k/v values and use the default
`1/sqrt(D)` scale with a causal mask. Sura explicitly invokes causal_attention
with precision: "auto". This measures eager rank-4 attention plus
its scalar-sum backward only. Sura's CPU reference reads
float32 storage through its documented double compute path; resident Sura CUDA
and PyTorch compute in float32. The default Sura path for T>=8 uses one f32
warp online-softmax forward launch and two deterministic fused recomputation
backward launches without an O(T^2) score workspace. This is not an end-to-end
Transformer benchmark, a shared-memory tiled/Tensor Core FlashAttention claim,
or framework-wide performance parity.
"@
    [IO.File]::WriteAllText($MarkdownOut, $markdown, $utf8)

    if ($sura.cuda.status -eq "ok" -and -not [bool]$sura.cuda.resident_transfer_free) {
        throw "Sura CUDA attention performed an unexpected timed H2D/D2H transfer"
    }
    if ($sura.cuda.status -eq "ok" -and $attentionCounterContractValid -ne $true) {
        throw "Sura CUDA attention launch counters violated the selected dispatch contract"
    }
    if ($sura.cuda.status -eq "ok" -and $torchCuda.status -eq "ok" -and
        $sameGpuDevice -ne $true) {
        throw "Sura and PyTorch CUDA results were not measured on the same GPU device"
    }
    if ($torch.status -eq "ok" -and [double]$inputChecksumError -gt 0.000001) {
        throw "Sura and PyTorch did not consume numerically identical benchmark inputs"
    }
    if ($sura.cuda.status -eq "ok" -and $suraCpuCudaValid -ne $true) {
        throw "Sura CPU/CUDA fingerprint error exceeded ValidationTolerance=$ValidationTolerance"
    }
    if ($torch.status -eq "ok" -and $suraTorchCpuValid -ne $true) {
        throw "Sura/PyTorch CPU fingerprint error exceeded ValidationTolerance=$ValidationTolerance"
    }
    Write-Host "sura_attention_benchmark: PASS"
    Write-Host "json: $([IO.Path]::GetFullPath($JsonOut))"
    Write-Host "markdown: $([IO.Path]::GetFullPath($MarkdownOut))"
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousEnvironment[$name], "Process")
    }
    $resolved = [IO.Path]::GetFullPath($inputPath)
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith("sura-attention-bench-") -and
        (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        Remove-Item -LiteralPath $resolved -Force
    }
}
