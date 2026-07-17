param(
    [string]$Engine = "",
    [string]$Python = "",
    [string]$CublasLibrary = "",
    [int]$MatrixSize = 1024,
    [int]$CorrectnessSize = 16,
    [int]$Warmup = 3,
    [int]$Runs = 10,
    [int]$MaxPreflightGpuUtilization = 5,
    [switch]$AllowBusyGpu,
    [string]$JsonOut = "",
    [string]$MarkdownOut = ""
)

$ErrorActionPreference = "Stop"
$suraSchema = "sura.cuda-mixed-compute-benchmark.v2"
$torchSchema = "sura.cuda-mixed-compute-benchmark.torch.v2"
$reportSchema = "sura.cuda-mixed-compute-benchmark-report.v2"
$root = Split-Path -Parent $PSScriptRoot
$benchmarkPath = Join-Path $root "benchmarks/cuda_mixed_compute_benchmark.sura"
$torchScript = Join-Path $root "benchmarks/cuda_mixed_compute_benchmark_torch.py"
$driverPath = $PSCommandPath
if ([string]::IsNullOrWhiteSpace($driverPath)) {
    $driverPath = $MyInvocation.MyCommand.Path
}

if ([string]::IsNullOrWhiteSpace($Engine)) {
    $Engine = Join-Path $root "SuraLanguage.exe"
}
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
if ([string]::IsNullOrWhiteSpace($JsonOut)) {
    $JsonOut = Join-Path $root "artifacts/cuda_mixed_compute_benchmark.json"
}
if ([string]::IsNullOrWhiteSpace($MarkdownOut)) {
    $MarkdownOut = Join-Path $root "artifacts/cuda_mixed_compute_benchmark.md"
}
$JsonOut = [IO.Path]::GetFullPath($JsonOut)
$MarkdownOut = [IO.Path]::GetFullPath($MarkdownOut)
if ([string]::Equals($JsonOut, $MarkdownOut, [StringComparison]::OrdinalIgnoreCase)) {
    throw "JsonOut and MarkdownOut must be different files"
}
if ($MatrixSize -lt 128 -or $MatrixSize % 16 -ne 0) {
    throw "MatrixSize must be at least 128 and divisible by 16"
}
if ($CorrectnessSize -lt 16 -or $CorrectnessSize % 16 -ne 0) {
    throw "CorrectnessSize must be at least 16 and divisible by 16"
}
if ($Warmup -lt 0 -or $Runs -lt 1) {
    throw "Warmup must be non-negative and Runs must be positive"
}
if ($MaxPreflightGpuUtilization -lt 0 -or
    $MaxPreflightGpuUtilization -gt 100) {
    throw "MaxPreflightGpuUtilization must be between 0 and 100"
}

$gpuPreflightUtilization = $null
$gpuPreflightPeakUtilization = $null
$gpuPreflightSamples = @()
$selectedDeviceIndex = 0
$requestedDevice = [Environment]::GetEnvironmentVariable("SURA_CUDA_DEVICE", "Process")
if (-not [string]::IsNullOrWhiteSpace($requestedDevice)) {
    $parsedDevice = 0
    if ([int]::TryParse($requestedDevice, [ref]$parsedDevice) -and
        $parsedDevice -ge 0) {
        $selectedDeviceIndex = $parsedDevice
    }
}
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

$pythonRequired = -not [string]::IsNullOrWhiteSpace($Python)
$pythonPath = $null
if ($pythonRequired) {
    $pythonPath = (Resolve-Path -LiteralPath $Python).Path
}

$previousCublasLibrary = $env:SURA_CUBLAS_LIBRARY
$benchmarkCublasLibrary = $null
if (-not [string]::IsNullOrWhiteSpace($CublasLibrary)) {
    $benchmarkCublasLibrary = (Resolve-Path -LiteralPath $CublasLibrary).Path
} elseif (-not [string]::IsNullOrWhiteSpace($previousCublasLibrary) -and
          (Test-Path -LiteralPath $previousCublasLibrary -PathType Leaf)) {
    $benchmarkCublasLibrary = (Resolve-Path -LiteralPath $previousCublasLibrary).Path
} elseif ($pythonPath) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $probeCode = "import pathlib, torch; p=pathlib.Path(torch.__file__).parent/'lib'; c=sorted(p.glob('cublas64_*.dll'), reverse=True); print(c[0] if c else '')"
        $probeOutput = & $pythonPath -c $probeCode 2>$null
        if ($LASTEXITCODE -eq 0) {
            $candidate = [string]($probeOutput | Select-Object -First 1)
            if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
                $benchmarkCublasLibrary = (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    } finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Get-NamedProperty {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-NestedProperty {
    param($Object, [string[]]$Path)
    $value = $Object
    foreach ($name in $Path) {
        $value = Get-NamedProperty $value $name
        if ($null -eq $value) { return $null }
    }
    return $value
}

function Test-FiniteNumber {
    param($Value)
    if ($null -eq $Value) { return $false }
    try { $number = [double]$Value } catch { return $false }
    return -not [double]::IsNaN($number) -and
           -not [double]::IsInfinity($number)
}

function Format-MarkdownCell {
    param([object]$Value)
    if ($null -eq $Value) { return "-" }
    return (([string]$Value) -replace "`r?`n", " " -replace "\|", "\|")
}

function Format-Milliseconds {
    param($Entry, [string]$Property)
    if ($null -eq $Entry -or [string]$Entry.status -ne "ok") { return "-" }
    return [Math]::Round([double](Get-NamedProperty $Entry $Property), 4)
}

function Get-FileSha256 {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Resolve-CublasHashPath {
    param([string]$Selected, [string]$Reported, [string]$EnginePath)
    $candidates = [Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($Selected)) { $candidates.Add($Selected) }
    if (-not [string]::IsNullOrWhiteSpace($Reported)) {
        $candidates.Add($Reported)
        $leaf = [IO.Path]::GetFileName($Reported)
        $candidates.Add((Join-Path (Split-Path -Parent $EnginePath) $leaf))
        if ($env:CUDA_PATH) {
            try {
                $candidates.Add((Join-Path (Join-Path $env:CUDA_PATH "bin") $leaf))
                $candidates.Add((Join-Path (Join-Path $env:CUDA_PATH "lib64") $leaf))
            } catch { }
        }
        foreach ($directory in @($env:PATH -split [IO.Path]::PathSeparator) +
                 @($env:LD_LIBRARY_PATH -split [IO.Path]::PathSeparator)) {
            if (-not [string]::IsNullOrWhiteSpace($directory)) {
                try { $candidates.Add((Join-Path $directory $leaf)) } catch { }
            }
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function New-TorchStatusReport {
    param([string]$Status, [string]$Reason)
    return [pscustomobject]@{
        schema = $torchSchema
        status = $Status
        reason = $Reason
        cases = $null
        correctness = $null
    }
}

function Add-TimingValidation {
    param(
        $Entry,
        [int]$ExpectedSamples,
        [string]$Label,
        [Collections.IDictionary]$Checks,
        [Collections.Generic.List[string]]$Failures
    )
    $raw = if ($null -eq $Entry) { @() } else { @(Get-NamedProperty $Entry "raw_ms") }
    $countValid = $raw.Count -eq $ExpectedSamples
    $samplesValid = $countValid
    foreach ($sample in $raw) {
        if (-not (Test-FiniteNumber $sample) -or [double]$sample -le 0.0) {
            $samplesValid = $false
        }
    }
    $medianValue = Get-NamedProperty $Entry "median_ms"
    $p95Value = Get-NamedProperty $Entry "p95_ms"
    $summaryValid = (Test-FiniteNumber $medianValue) -and
        (Test-FiniteNumber $p95Value) -and
        [double]$medianValue -gt 0.0 -and [double]$p95Value -gt 0.0
    $Checks["${Label}_raw_sample_count"] = $countValid
    $Checks["${Label}_raw_samples_finite_positive"] = $samplesValid
    $Checks["${Label}_summary_finite_positive"] = $summaryValid
    if (-not $countValid) { $Failures.Add("$Label raw sample count is not $ExpectedSamples") }
    if (-not $samplesValid) { $Failures.Add("$Label contains a non-finite or non-positive raw sample") }
    if (-not $summaryValid) { $Failures.Add("$Label median/p95 is non-finite or non-positive") }
}

function Add-FingerprintFiniteValidation {
    param(
        $Fingerprint,
        [string]$Label,
        [Collections.Generic.List[string]]$Failures
    )
    $metricPaths = @(
        @("sum"), @("l1"), @("l2"),
        @("samples", "first"), @("samples", "center"), @("samples", "last")
    )
    $valid = $true
    foreach ($path in $metricPaths) {
        $value = Get-NestedProperty $Fingerprint $path
        if (-not (Test-FiniteNumber $value)) {
            $valid = $false
            $Failures.Add("$Label fingerprint $($path -join '.') is missing or non-finite")
        }
    }
    return $valid
}

function Compare-Fingerprints {
    param(
        $SuraFingerprint,
        $TorchFingerprint,
        [Collections.IDictionary]$Tolerance,
        [string]$Label,
        [Collections.Generic.List[string]]$Failures
    )
    $metricSpecs = @(
        [pscustomobject]@{ name = "sum"; path = @("sum"); sample = $false },
        [pscustomobject]@{ name = "l1"; path = @("l1"); sample = $false },
        [pscustomobject]@{ name = "l2"; path = @("l2"); sample = $false },
        [pscustomobject]@{ name = "sample_first"; path = @("samples", "first"); sample = $true },
        [pscustomobject]@{ name = "sample_center"; path = @("samples", "center"); sample = $true },
        [pscustomobject]@{ name = "sample_last"; path = @("samples", "last"); sample = $true }
    )
    $result = [ordered]@{}
    foreach ($metric in $metricSpecs) {
        $suraValue = Get-NestedProperty $SuraFingerprint $metric.path
        $torchValue = Get-NestedProperty $TorchFingerprint $metric.path
        $finite = (Test-FiniteNumber $suraValue) -and (Test-FiniteNumber $torchValue)
        $absoluteTolerance = if ($metric.sample) {
            [double]$Tolerance.sample_absolute
        } else {
            [double]$Tolerance.aggregate_absolute
        }
        $relativeTolerance = [double]$Tolerance.relative
        $absoluteError = $null
        $limit = $null
        $matched = $false
        if ($finite) {
            $absoluteError = [Math]::Abs([double]$suraValue - [double]$torchValue)
            $limit = $absoluteTolerance +
                $relativeTolerance * [Math]::Abs([double]$torchValue)
            $matched = $absoluteError -le $limit
        }
        $result[$metric.name] = [ordered]@{
            sura = $suraValue
            pytorch = $torchValue
            absolute_error = $absoluteError
            limit = $limit
            matched = $matched
        }
        if (-not $matched) {
            $Failures.Add("$Label fingerprint $($metric.name) exceeded tolerance")
        }
    }
    return [pscustomobject]$result
}

function Get-DispatchMode {
    param($Stats, [int64]$Expected, [bool]$Mixed)
    if (-not $Mixed) {
        if ([int64]$Stats.cublas_matmul_launches -eq $Expected) { return "cublas_sgemm" }
        if ([int64]$Stats.reference_matmul_launches -eq $Expected) { return "reference_ptx" }
        return "mixed_f32_backends"
    }
    $fast = [int64]$Stats.cublas_fast_matmul_launches
    $fallback = [int64]$Stats.mixed_matmul_fallback_launches
    if ($fast -eq $Expected) { return "cublas_gemmex_fast" }
    if ($fallback -eq $Expected) { return "reference_ptx_fallback" }
    return "mixed_fast_and_fallback"
}

function New-ValidatedTempFile {
    param([string]$Destination, [string]$Content, [string]$Kind)
    $parent = Split-Path -Parent $Destination
    if ([string]::IsNullOrWhiteSpace($parent)) {
        $parent = [Environment]::CurrentDirectory
    }
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $leaf = [IO.Path]::GetFileName($Destination)
    $temp = Join-Path $parent (".$leaf." + [guid]::NewGuid().ToString("N") + ".tmp")
    $utf8 = [Text.UTF8Encoding]::new($false)
    try {
        [IO.File]::WriteAllText($temp, $Content, $utf8)
        if ($Kind -eq "json") {
            $parsed = Get-Content -Raw -LiteralPath $temp | ConvertFrom-Json
            if ([string]$parsed.schema -ne $reportSchema) {
                throw "temporary JSON report schema validation failed"
            }
        } else {
            $observed = Get-Content -Raw -LiteralPath $temp
            if (-not $observed.StartsWith("# Sura CUDA Mixed-Compute Benchmark") -or
                $observed.Length -lt 100) {
                throw "temporary Markdown report validation failed"
            }
        }
        return $temp
    } catch {
        if (Test-Path -LiteralPath $temp -PathType Leaf) {
            Remove-Item -LiteralPath $temp -Force
        }
        throw
    }
}

function Publish-AtomicTempFile {
    param([string]$Temp, [string]$Destination)
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $parent = Split-Path -Parent $Destination
        $leaf = [IO.Path]::GetFileName($Destination)
        $backup = Join-Path $parent (
            ".$leaf." + [guid]::NewGuid().ToString("N") + ".replace-backup")
        try {
            [IO.File]::Replace($Temp, $Destination, $backup, $true)
        } finally {
            if (Test-Path -LiteralPath $backup -PathType Leaf) {
                Remove-Item -LiteralPath $backup -Force
            }
        }
    } else {
        [IO.File]::Move($Temp, $Destination)
    }
}

$fingerprintTolerances = [ordered]@{
    float32 = [ordered]@{ aggregate_absolute = 0.01; sample_absolute = 0.001; relative = 0.0001 }
    float16 = [ordered]@{ aggregate_absolute = 0.5; sample_absolute = 0.02; relative = 0.005 }
    bfloat16 = [ordered]@{ aggregate_absolute = 5.0; sample_absolute = 0.25; relative = 0.025 }
}

$sourceHashesBefore = [ordered]@{
    sura_benchmark = Get-FileSha256 $benchmarkPath
    pytorch_helper = Get-FileSha256 $torchScript
    powershell_driver = Get-FileSha256 $driverPath
}
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$inputPath = Join-Path $tempRoot (
    "sura-mixed-compute-bench-" + [guid]::NewGuid().ToString("N") + ".safetensors")
$environmentNames = @(
    "SURA_MIXED_BENCH_INPUTS",
    "SURA_MIXED_BENCH_SIZE",
    "SURA_MIXED_BENCH_CORRECTNESS_SIZE",
    "SURA_MIXED_BENCH_WARMUP",
    "SURA_MIXED_BENCH_RUNS",
    "SURA_CUBLAS_LIBRARY",
    "NVIDIA_TF32_OVERRIDE"
)
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
    $env:SURA_MIXED_BENCH_INPUTS = $inputPath
    $env:SURA_MIXED_BENCH_SIZE = [string]$MatrixSize
    $env:SURA_MIXED_BENCH_CORRECTNESS_SIZE = [string]$CorrectnessSize
    $env:SURA_MIXED_BENCH_WARMUP = [string]$Warmup
    $env:SURA_MIXED_BENCH_RUNS = [string]$Runs
    $env:NVIDIA_TF32_OVERRIDE = "0"
    if ($benchmarkCublasLibrary) {
        $env:SURA_CUBLAS_LIBRARY = $benchmarkCublasLibrary
    } else {
        Remove-Item Env:SURA_CUBLAS_LIBRARY -ErrorAction SilentlyContinue
    }

    $suraOutput = & $enginePath $benchmarkPath 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "Sura mixed-compute benchmark failed:`n$($suraOutput -join [Environment]::NewLine)"
    }
    $suraLine = $suraOutput |
        Where-Object { $_.TrimStart().StartsWith("{") } |
        Select-Object -Last 1
    if (-not $suraLine) { throw "Sura mixed-compute benchmark emitted no JSON" }
    $sura = $suraLine | ConvertFrom-Json

    $inputFileHash = Get-FileSha256 $inputPath
    $torch = New-TorchStatusReport "not_requested" (
        "PyTorch comparison was not requested; pass -Python <python.exe> to require it")
    if ($pythonRequired) {
        if (-not $inputFileHash) {
            $torch = New-TorchStatusReport "error" (
                "Sura did not create the shared Safetensors input file")
        } else {
            $deviceIndex = if ($sura.status -eq "ok") { [int]$sura.device_index } else { 0 }
            $torchArgs = @(
                $torchScript, $inputPath,
                "--matrix-size", [string]$MatrixSize,
                "--correctness-size", [string]$CorrectnessSize,
                "--warmup", [string]$Warmup,
                "--runs", [string]$Runs,
                "--device-index", [string]$deviceIndex
            )
            $oldPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $torchOutput = & $pythonPath @torchArgs 2>&1 | ForEach-Object { "$_" }
                $torchExitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $oldPreference
            }
            if ($torchExitCode -eq 0) {
                $torchLine = $torchOutput |
                    Where-Object { $_.TrimStart().StartsWith("{") } |
                    Select-Object -Last 1
                if ($torchLine) {
                    $torch = $torchLine | ConvertFrom-Json
                } else {
                    $torch = New-TorchStatusReport "error" (
                        "required PyTorch helper emitted no JSON")
                }
            } else {
                $torch = New-TorchStatusReport "error" (
                    "required PyTorch helper failed: " +
                    ($torchOutput -join [Environment]::NewLine))
            }
        }
    }

    $validationFailures = [Collections.Generic.List[string]]::new()
    $validationWarnings = [Collections.Generic.List[string]]::new()
    if ($gpuWasBusy) {
        $validationWarnings.Add("GPU preflight utilization was $gpuPreflightUtilization%; timings are diagnostic and not public-performance evidence")
    } elseif ($null -eq $gpuPreflightUtilization) {
        $validationWarnings.Add("GPU preflight utilization could not be measured; timings are not public-performance evidence")
    }
    $caseValidation = [ordered]@{}
    $torchCaseValidation = [ordered]@{}
    $dispatchValidation = [ordered]@{}
    $fingerprintValidation = [ordered]@{}
    $expectedMatrixElements = [int64]$MatrixSize * [int64]$MatrixSize
    $expectedMatrixBytes = $expectedMatrixElements * 4L
    $expectedCorrectnessElements = [int64]$CorrectnessSize * [int64]$CorrectnessSize
    $expectedCorrectnessBytes = $expectedCorrectnessElements * 4L

    if ([string]$sura.schema -ne $suraSchema) {
        $validationFailures.Add("Sura schema mismatch: expected $suraSchema")
    }
    $suraOk = [string]$sura.status -eq "ok"
    if (-not $suraOk) {
        $validationFailures.Add("Sura CUDA benchmark unavailable: $($sura.reason)")
    }
    if (-not $inputFileHash) {
        $validationFailures.Add("shared Safetensors input file is missing")
    }

    $computeMajor = $null
    $computeMinor = $null
    if ($suraOk -and [string]$sura.compute_capability -match '^(\d+)\.(\d+)$') {
        $computeMajor = [int]$Matches[1]
        $computeMinor = [int]$Matches[2]
    } elseif ($suraOk) {
        $validationFailures.Add("Sura compute capability is missing or malformed")
    }

    if ($suraOk) {
        if ([int]$sura.matrix_size -ne $MatrixSize -or
            [int]$sura.correctness_size -ne $CorrectnessSize -or
            [int]$sura.warmup -ne $Warmup -or [int]$sura.runs -ne $Runs) {
            $validationFailures.Add("Sura benchmark parameters do not match the driver")
        }
        if ([string]$sura.nvidia_tf32_override -ne "0") {
            $validationFailures.Add("Sura did not observe NVIDIA_TF32_OVERRIDE=0")
        }
        if ([string]$sura.sura_storage_dtype -ne "float32" -or
            [string]$sura.sura_gradient_dtype -ne "float32") {
            $validationFailures.Add("Sura limits do not report float32 CUDA tensor/gradient storage")
        }

        foreach ($dtype in @("float32", "float16", "bfloat16")) {
            $case = Get-NamedProperty $sura.cases $dtype
            $correctness = Get-NamedProperty $sura.correctness $dtype
            if ($null -eq $case -or [string]$case.status -ne "ok") {
                $validationFailures.Add("Sura $dtype performance case is missing or unavailable")
                continue
            }
            if ($null -eq $correctness -or [string]$correctness.status -ne "ok") {
                $validationFailures.Add("Sura $dtype correctness case is missing or unavailable")
                continue
            }

            $forwardStats = $case.forward.stats_before_read
            $forwardAfterRead = $case.forward.stats_after_read
            $trainingStats = $case.forward_backward.stats_before_read
            $trainingAfterRead = $case.forward_backward.stats_after_read
            $correctnessStats = $correctness.stats_before_read
            $forwardExpected = [int64]$Runs
            $trainingExpected = [int64]$Runs * 3L
            $correctnessExpected = 3L
            $counterName = switch ($dtype) {
                "float32" { "float32_matmul_launches" }
                "float16" { "float16_matmul_launches" }
                default { "bfloat16_matmul_launches" }
            }
            $otherCounters = @(
                "float32_matmul_launches",
                "float16_matmul_launches",
                "bfloat16_matmul_launches"
            ) | Where-Object { $_ -ne $counterName }
            $inputGradientInfo = $case.forward_backward.input_gradient_info
            $weightGradientInfo = $case.forward_backward.weight_gradient_info
            $correctnessContract = $correctness.tensor_contract
            $correctnessInputGradientInfo = $correctnessContract.input_gradient_info
            $correctnessWeightGradientInfo = $correctnessContract.weight_gradient_info
            $suraCorrectnessShape = @($correctness.shape)
            $suraTrainingFingerprint = $correctness.training_fingerprint

            $checks = [ordered]@{
                forward_transfer_free = ([int64]$forwardStats.h2d_bytes -eq 0 -and [int64]$forwardStats.d2h_bytes -eq 0)
                training_transfer_free = ([int64]$trainingStats.h2d_bytes -eq 0 -and [int64]$trainingStats.d2h_bytes -eq 0)
                correctness_transfer_free_before_observation = ([int64]$correctnessStats.h2d_bytes -eq 0 -and [int64]$correctnessStats.d2h_bytes -eq 0)
                forward_checksum_observation_boundary = ([int64]$forwardAfterRead.d2h_bytes -eq 4 -and [int64]$forwardAfterRead.reduction_launches -eq ([int64]$forwardStats.reduction_launches + 1L))
                training_checksum_observation_boundary = ([int64]$trainingAfterRead.d2h_bytes -eq 4 -and [int64]$trainingAfterRead.reduction_launches -eq ([int64]$trainingStats.reduction_launches + 1L))
                forward_matmul_count = [int64]$forwardStats.matmul_launches -eq $forwardExpected
                training_matmul_count = [int64]$trainingStats.matmul_launches -eq $trainingExpected
                correctness_matmul_count = [int64]$correctnessStats.matmul_launches -eq $correctnessExpected
                forward_selected_dtype_count = [int64](Get-NamedProperty $forwardStats $counterName) -eq $forwardExpected
                training_selected_dtype_count = [int64](Get-NamedProperty $trainingStats $counterName) -eq $trainingExpected
                correctness_selected_dtype_count = [int64](Get-NamedProperty $correctnessStats $counterName) -eq $correctnessExpected
                forward_backend_partition = ([int64]$forwardStats.cublas_matmul_launches + [int64]$forwardStats.reference_matmul_launches) -eq $forwardExpected
                training_backend_partition = ([int64]$trainingStats.cublas_matmul_launches + [int64]$trainingStats.reference_matmul_launches) -eq $trainingExpected
                correctness_backend_partition = ([int64]$correctnessStats.cublas_matmul_launches + [int64]$correctnessStats.reference_matmul_launches) -eq $correctnessExpected
                actual_tensor_dtypes_float32 = ([string]$case.forward.input_dtype -eq "float32" -and [string]$case.forward.weight_dtype -eq "float32" -and [string]$case.forward.output_dtype -eq "float32" -and [string]$case.forward_backward.output_dtype -eq "float32")
                actual_tensor_storage_float32 = ([int64]$case.forward.input_storage_bytes -eq $expectedMatrixBytes -and [int64]$case.forward.weight_storage_bytes -eq $expectedMatrixBytes -and [int64]$case.forward.output_storage_bytes -eq $expectedMatrixBytes -and [int64]$case.forward_backward.output_storage_bytes -eq $expectedMatrixBytes)
                input_gradient_contract = ([bool]$inputGradientInfo.present -and [string]$inputGradientInfo.dtype -eq "float32" -and [string]$inputGradientInfo.device -eq "cuda:$($sura.device_index)" -and [int64]$inputGradientInfo.elements -eq $expectedMatrixElements -and [int64]$inputGradientInfo.storage_bytes -eq $expectedMatrixBytes -and [double]$inputGradientInfo.scale -eq 1.0 -and [bool]$inputGradientInfo.optimizer_ready)
                weight_gradient_contract = ([bool]$weightGradientInfo.present -and [string]$weightGradientInfo.dtype -eq "float32" -and [string]$weightGradientInfo.device -eq "cuda:$($sura.device_index)" -and [int64]$weightGradientInfo.elements -eq $expectedMatrixElements -and [int64]$weightGradientInfo.storage_bytes -eq $expectedMatrixBytes -and [double]$weightGradientInfo.scale -eq 1.0 -and [bool]$weightGradientInfo.optimizer_ready)
                correctness_tensor_contract = ([string]$correctnessContract.input_dtype -eq "float32" -and [string]$correctnessContract.weight_dtype -eq "float32" -and [string]$correctnessContract.output_dtype -eq "float32" -and [int64]$correctnessContract.input_storage_bytes -eq $expectedCorrectnessBytes -and [int64]$correctnessContract.weight_storage_bytes -eq $expectedCorrectnessBytes -and [int64]$correctnessContract.output_storage_bytes -eq $expectedCorrectnessBytes)
                correctness_shape = ($suraCorrectnessShape.Count -eq 2 -and [int]$suraCorrectnessShape[0] -eq $CorrectnessSize -and [int]$suraCorrectnessShape[1] -eq $CorrectnessSize)
                correctness_input_gradient_contract = ([bool]$correctnessInputGradientInfo.present -and [string]$correctnessInputGradientInfo.dtype -eq "float32" -and [int64]$correctnessInputGradientInfo.elements -eq $expectedCorrectnessElements -and [int64]$correctnessInputGradientInfo.storage_bytes -eq $expectedCorrectnessBytes -and [double]$correctnessInputGradientInfo.scale -eq 1.0)
                correctness_weight_gradient_contract = ([bool]$correctnessWeightGradientInfo.present -and [string]$correctnessWeightGradientInfo.dtype -eq "float32" -and [int64]$correctnessWeightGradientInfo.elements -eq $expectedCorrectnessElements -and [int64]$correctnessWeightGradientInfo.storage_bytes -eq $expectedCorrectnessBytes -and [double]$correctnessWeightGradientInfo.scale -eq 1.0)
                correctness_training_fingerprint_present = ($null -ne $suraTrainingFingerprint -and $null -ne $suraTrainingFingerprint.output -and $null -ne $suraTrainingFingerprint.input_gradient -and $null -ne $suraTrainingFingerprint.weight_gradient)
                forward_output_checksum_finite = Test-FiniteNumber $case.forward.output_checksum
                training_output_checksum_finite = Test-FiniteNumber $case.forward_backward.output_checksum
            }
            Add-TimingValidation $case.forward $Runs "sura_${dtype}_forward" $checks $validationFailures
            Add-TimingValidation $case.forward_backward $Runs "sura_${dtype}_forward_backward" $checks $validationFailures
            foreach ($otherCounter in $otherCounters) {
                $checks["forward_zero_$otherCounter"] = [int64](Get-NamedProperty $forwardStats $otherCounter) -eq 0
                $checks["training_zero_$otherCounter"] = [int64](Get-NamedProperty $trainingStats $otherCounter) -eq 0
                $checks["correctness_zero_$otherCounter"] = [int64](Get-NamedProperty $correctnessStats $otherCounter) -eq 0
            }

            $mixed = $dtype -ne "float32"
            if (-not $mixed) {
                $checks["forward_no_mixed_dispatch"] = ([int64]$forwardStats.cublas_fast_matmul_launches -eq 0 -and [int64]$forwardStats.mixed_matmul_fallback_launches -eq 0)
                $checks["training_no_mixed_dispatch"] = ([int64]$trainingStats.cublas_fast_matmul_launches -eq 0 -and [int64]$trainingStats.mixed_matmul_fallback_launches -eq 0)
                $checks["correctness_no_mixed_dispatch"] = ([int64]$correctnessStats.cublas_fast_matmul_launches -eq 0 -and [int64]$correctnessStats.mixed_matmul_fallback_launches -eq 0)
            } else {
                $checks["forward_mixed_dispatch_partition"] = ([int64]$forwardStats.cublas_fast_matmul_launches + [int64]$forwardStats.mixed_matmul_fallback_launches) -eq $forwardExpected
                $checks["training_mixed_dispatch_partition"] = ([int64]$trainingStats.cublas_fast_matmul_launches + [int64]$trainingStats.mixed_matmul_fallback_launches) -eq $trainingExpected
                $checks["correctness_mixed_dispatch_partition"] = ([int64]$correctnessStats.cublas_fast_matmul_launches + [int64]$correctnessStats.mixed_matmul_fallback_launches) -eq $correctnessExpected
            }

            $fingerprintsFinite = $true
            foreach ($fingerprintName in @("output", "input_gradient", "weight_gradient")) {
                if (-not (Add-FingerprintFiniteValidation (
                        Get-NamedProperty $suraTrainingFingerprint $fingerprintName) (
                        "Sura $dtype $fingerprintName") $validationFailures)) {
                    $fingerprintsFinite = $false
                }
            }
            $checks["correctness_fingerprints_finite"] = $fingerprintsFinite

            foreach ($entry in $checks.GetEnumerator()) {
                if (-not [bool]$entry.Value) {
                    $validationFailures.Add("Sura $dtype validation failed: $($entry.Key)")
                }
            }
            $caseValidation[$dtype] = [pscustomobject]$checks

            $architectureFastEligible = $false
            if ($mixed -and $null -ne $computeMajor) {
                $architectureFastEligible = if ($dtype -eq "float16") {
                    $computeMajor -ge 7
                } else {
                    $computeMajor -ge 8
                }
            }
            $staticFastEligible = $architectureFastEligible -and
                [bool]$sura.cublas_gemm_ex_available
            $dispatch = [ordered]@{
                compute_capability = [string]$sura.compute_capability
                cublas_gemm_ex_symbol = [bool]$sura.cublas_gemm_ex_available
                architecture_fast_eligible = $architectureFastEligible
                static_fast_eligible = $staticFastEligible
                fallback_is_legal = $mixed
                forward = Get-DispatchMode $forwardStats $forwardExpected $mixed
                forward_backward = Get-DispatchMode $trainingStats $trainingExpected $mixed
                correctness = Get-DispatchMode $correctnessStats $correctnessExpected $mixed
                forward_fast = [int64]$forwardStats.cublas_fast_matmul_launches
                forward_fallback = [int64]$forwardStats.mixed_matmul_fallback_launches
                forward_backward_fast = [int64]$trainingStats.cublas_fast_matmul_launches
                forward_backward_fallback = [int64]$trainingStats.mixed_matmul_fallback_launches
                correctness_fast = [int64]$correctnessStats.cublas_fast_matmul_launches
                correctness_fallback = [int64]$correctnessStats.mixed_matmul_fallback_launches
            }
            if ($staticFastEligible -and $mixed -and
                ([int64]$forwardStats.mixed_matmul_fallback_launches -gt 0 -or
                 [int64]$trainingStats.mixed_matmul_fallback_launches -gt 0 -or
                 [int64]$correctnessStats.mixed_matmul_fallback_launches -gt 0)) {
                $validationWarnings.Add("$dtype used a legal PTX fallback despite static GemmEx/architecture eligibility")
            }
            $dispatchValidation[$dtype] = [pscustomobject]$dispatch
        }
        if ($benchmarkCublasLibrary -and -not [bool]$sura.cublas_available) {
            $validationFailures.Add("selected cuBLAS library was not loaded by Sura")
        } elseif ($benchmarkCublasLibrary -and -not [bool]$sura.cublas_gemm_ex_available) {
            $validationWarnings.Add("selected cuBLAS library lacks cublasGemmEx; mixed compute legally uses PTX fallback")
        }
    }

    if ($pythonRequired) {
        if ([string]$torch.schema -ne $torchSchema) {
            $validationFailures.Add("PyTorch helper schema mismatch: expected $torchSchema")
        }
        if ([string]$torch.status -ne "ok") {
            $validationFailures.Add("required PyTorch comparison is not fully available: $($torch.reason)")
        }
    }
    $torchOk = $pythonRequired -and [string]$torch.schema -eq $torchSchema -and
        [string]$torch.status -eq "ok"

    $sameDeviceName = $null
    $sameDeviceIndex = $null
    $sameComputeCapability = $null
    $sameDeviceUuid = $null
    $samePciBusId = $null
    $sameGpuDevice = $null
    if ($suraOk -and $torchOk) {
        $sameDeviceName = [string]$sura.device -eq [string]$torch.device_name
        $sameDeviceIndex = [int]$sura.device_index -eq [int]$torch.device_index
        $sameComputeCapability = [string]$sura.compute_capability -eq
            [string]$torch.compute_capability
        if ($sura.device_uuid -and $torch.device_uuid) {
            $sameDeviceUuid = [string]$sura.device_uuid -eq [string]$torch.device_uuid
        }
        if ($sura.pci_bus_id -and $torch.pci_bus_id) {
            $samePciBusId = [string]$sura.pci_bus_id -eq [string]$torch.pci_bus_id
        }
        $sameGpuDevice = $sameDeviceName -and $sameDeviceIndex -and
            $sameComputeCapability -and
            ($null -eq $sameDeviceUuid -or $sameDeviceUuid) -and
            ($null -eq $samePciBusId -or $samePciBusId)
        if (-not $sameDeviceName) { $validationFailures.Add("Sura/PyTorch GPU names differ") }
        if (-not $sameDeviceIndex) { $validationFailures.Add("Sura/PyTorch CUDA device indices differ") }
        if (-not $sameComputeCapability) { $validationFailures.Add("Sura/PyTorch compute capabilities differ") }
        if ($sameDeviceUuid -eq $false) { $validationFailures.Add("Sura/PyTorch GPU UUIDs differ") }
        if ($samePciBusId -eq $false) { $validationFailures.Add("Sura/PyTorch PCI bus ids differ") }
        if ([string]$torch.nvidia_tf32_override -ne "0") {
            $validationFailures.Add("PyTorch helper did not observe NVIDIA_TF32_OVERRIDE=0")
        }
        if ([string]$torch.float32_matmul_precision -ne "highest") {
            $validationFailures.Add("PyTorch float32 matmul precision is not highest")
        }
        if ([string]$torch.float32_matmul_policy -notin @("ieee", "allow_tf32=false") -or
            [bool]$torch.allow_tf32_effective) {
            $validationFailures.Add("PyTorch effective float32 matmul policy still permits TF32")
        }
        foreach ($reductionName in @(
                "allow_fp16_reduced_precision_reduction",
                "allow_bf16_reduced_precision_reduction")) {
            $value = Get-NamedProperty $torch.reduced_precision_reduction $reductionName
            if ($null -ne $value -and [bool]$value) {
                $validationFailures.Add("PyTorch $reductionName was not disabled")
            }
        }
        if ([string]$torch.input_file_sha256 -ne [string]$inputFileHash) {
            $validationFailures.Add("PyTorch did not hash the byte-identical Safetensors input")
        }
        if ([int]$torch.matrix_size -ne $MatrixSize -or
            [int]$torch.correctness_size -ne $CorrectnessSize -or
            [int]$torch.warmup -ne $Warmup -or [int]$torch.runs -ne $Runs) {
            $validationFailures.Add("PyTorch benchmark parameters do not match the driver")
        }

        foreach ($dtype in @("float32", "float16", "bfloat16")) {
            $torchCase = Get-NamedProperty $torch.cases $dtype
            $torchCorrectness = Get-NamedProperty $torch.correctness $dtype
            if ($null -eq $torchCase -or [string]$torchCase.status -ne "ok") {
                $validationFailures.Add("required PyTorch $dtype performance case is unavailable")
                continue
            }
            if ($null -eq $torchCorrectness -or [string]$torchCorrectness.status -ne "ok") {
                $validationFailures.Add("required PyTorch $dtype correctness case is unavailable")
                continue
            }
            $torchChecks = [ordered]@{}
            Add-TimingValidation $torchCase.forward $Runs "pytorch_${dtype}_forward" $torchChecks $validationFailures
            Add-TimingValidation $torchCase.forward_backward $Runs "pytorch_${dtype}_forward_backward" $torchChecks $validationFailures
            $bytesPerElement = if ($dtype -eq "float32") { 4L } else { 2L }
            $expectedTorchBytes = $expectedMatrixElements * $bytesPerElement
            $expectedTorchCorrectnessBytes = $expectedCorrectnessElements * $bytesPerElement
            $torchContract = $torchCorrectness.tensor_contract
            $torchCorrectnessShape = @($torchCorrectness.shape)
            $torchTrainingFingerprint = $torchCorrectness.training_fingerprint
            $torchChecks["native_storage_contract"] = (
                [string]$torchCase.forward.input_dtype -eq $dtype -and
                [string]$torchCase.forward.weight_dtype -eq $dtype -and
                [string]$torchCase.forward.output_dtype -eq $dtype -and
                [int64]$torchCase.forward.input_storage_bytes -eq $expectedTorchBytes -and
                [int64]$torchCase.forward.weight_storage_bytes -eq $expectedTorchBytes -and
                [int64]$torchCase.forward.output_storage_bytes -eq $expectedTorchBytes -and
                [string]$torchCase.forward_backward.input_gradient_dtype -eq $dtype -and
                [string]$torchCase.forward_backward.weight_gradient_dtype -eq $dtype -and
                [int64]$torchCase.forward_backward.input_gradient_storage_bytes -eq $expectedTorchBytes -and
                [int64]$torchCase.forward_backward.weight_gradient_storage_bytes -eq $expectedTorchBytes)
            $torchChecks["correctness_native_storage_contract"] = (
                [string]$torchContract.input_dtype -eq $dtype -and
                [string]$torchContract.weight_dtype -eq $dtype -and
                [string]$torchContract.output_dtype -eq $dtype -and
                [string]$torchContract.input_gradient_dtype -eq $dtype -and
                [string]$torchContract.weight_gradient_dtype -eq $dtype -and
                [int64]$torchContract.input_storage_bytes -eq $expectedTorchCorrectnessBytes -and
                [int64]$torchContract.weight_storage_bytes -eq $expectedTorchCorrectnessBytes -and
                [int64]$torchContract.output_storage_bytes -eq $expectedTorchCorrectnessBytes -and
                [int64]$torchContract.input_gradient_storage_bytes -eq $expectedTorchCorrectnessBytes -and
                [int64]$torchContract.weight_gradient_storage_bytes -eq $expectedTorchCorrectnessBytes)
            $torchChecks["correctness_shape"] = (
                $torchCorrectnessShape.Count -eq 2 -and
                [int]$torchCorrectnessShape[0] -eq $CorrectnessSize -and
                [int]$torchCorrectnessShape[1] -eq $CorrectnessSize)
            $torchChecks["correctness_training_fingerprint_present"] = (
                $null -ne $torchTrainingFingerprint -and
                $null -ne $torchTrainingFingerprint.output -and
                $null -ne $torchTrainingFingerprint.input_gradient -and
                $null -ne $torchTrainingFingerprint.weight_gradient)
            foreach ($entry in $torchChecks.GetEnumerator()) {
                if (-not [bool]$entry.Value) {
                    $validationFailures.Add("PyTorch $dtype validation failed: $($entry.Key)")
                }
            }
            $torchCaseValidation[$dtype] = [pscustomobject]$torchChecks

            $suraCorrectness = Get-NamedProperty $sura.correctness $dtype
            $suraCorrectnessFingerprint = $suraCorrectness.training_fingerprint
            $torchCorrectnessFingerprint = $torchCorrectness.training_fingerprint
            $tolerance = $fingerprintTolerances[$dtype]
            $fingerprintFailureCountBefore = $validationFailures.Count
            $dtypeFingerprint = [ordered]@{
                tolerance = [pscustomobject]$tolerance
                output = Compare-Fingerprints $suraCorrectnessFingerprint.output $torchCorrectnessFingerprint.output $tolerance "$dtype output" $validationFailures
                input_gradient = Compare-Fingerprints $suraCorrectnessFingerprint.input_gradient $torchCorrectnessFingerprint.input_gradient $tolerance "$dtype input_gradient" $validationFailures
                weight_gradient = Compare-Fingerprints $suraCorrectnessFingerprint.weight_gradient $torchCorrectnessFingerprint.weight_gradient $tolerance "$dtype weight_gradient" $validationFailures
            }
            $dtypeFingerprint["passed"] =
                $validationFailures.Count -eq $fingerprintFailureCountBefore
            $fingerprintValidation[$dtype] = [pscustomobject]$dtypeFingerprint
        }
    }

    $checksumValidation = [ordered]@{}
    if ($suraOk -and $torchOk) {
        foreach ($scope in @("performance", "correctness")) {
            $suraChecksums = if ($scope -eq "performance") {
                $sura.input_checksums
            } else {
                $sura.correctness_input_checksums
            }
            $torchChecksums = if ($scope -eq "performance") {
                $torch.input_checksums
            } else {
                $torch.correctness_input_checksums
            }
            foreach ($name in @("input", "weight", "seed")) {
                $suraChecksum = [double](Get-NamedProperty $suraChecksums $name)
                $torchChecksum = [double](Get-NamedProperty $torchChecksums $name)
                $absoluteError = [Math]::Abs($suraChecksum - $torchChecksum)
                $tolerance = [Math]::Max(0.01, [Math]::Abs($suraChecksum) * 0.000001)
                $key = "${scope}_$name"
                $checksumValidation[$key] = [ordered]@{
                    sura = $suraChecksum
                    pytorch = $torchChecksum
                    absolute_error = $absoluteError
                    tolerance = $tolerance
                    matched = $absoluteError -le $tolerance
                }
                if ($absoluteError -gt $tolerance) {
                    $validationFailures.Add("shared input checksum mismatch: $key")
                }
            }
        }
    }

    $comparisons = [ordered]@{}
    foreach ($dtype in @("float32", "float16", "bfloat16")) {
        $suraCase = if ($suraOk) { Get-NamedProperty $sura.cases $dtype } else { $null }
        $torchCase = if ($torchOk) { Get-NamedProperty $torch.cases $dtype } else { $null }
        if ($null -ne $suraCase -and [string]$suraCase.status -eq "ok" -and
            $null -ne $torchCase -and [string]$torchCase.status -eq "ok") {
            $suraOutputChecksum = [double]$suraCase.forward.output_checksum
            $torchOutputChecksum = [double]$torchCase.forward.output_checksum
            $outputChecksumError = [Math]::Abs($suraOutputChecksum - $torchOutputChecksum)
            $outputChecksumRelativeError = $outputChecksumError /
                [Math]::Max(1.0, [Math]::Abs($torchOutputChecksum))
            $comparisons[$dtype] = [ordered]@{
                status = "ok"
                same_gpu = $sameGpuDevice
                same_source_input_sha256 = [string]$inputFileHash
                sura_compute_dtype = $dtype
                sura_storage_dtype = "float32"
                sura_output_dtype = [string]$suraCase.forward.output_dtype
                pytorch_compute_dtype = $dtype
                pytorch_storage_dtype = [string]$torchCase.storage_dtype
                pytorch_output_dtype = [string]$torchCase.forward.output_dtype
                storage_models_equal = $dtype -eq "float32"
                forward_output_checksum_sura = $suraOutputChecksum
                forward_output_checksum_pytorch = $torchOutputChecksum
                forward_output_checksum_absolute_error = $outputChecksumError
                forward_output_checksum_relative_error = $outputChecksumRelativeError
                forward_sura_over_pytorch = [double]$suraCase.forward.median_ms / [double]$torchCase.forward.median_ms
                forward_backward_sura_over_pytorch = [double]$suraCase.forward_backward.median_ms / [double]$torchCase.forward_backward.median_ms
            }
        } elseif (-not $pythonRequired) {
            $comparisons[$dtype] = [ordered]@{
                status = "not_requested"
                reason = "PyTorch comparison requires explicit -Python"
            }
        } else {
            $reason = if ($null -ne $torchCase) { [string]$torchCase.reason } else { [string]$torch.reason }
            $comparisons[$dtype] = [ordered]@{
                status = "unavailable"
                reason = $reason
            }
        }
    }

    $sourceHashesAfter = [ordered]@{
        sura_benchmark = Get-FileSha256 $benchmarkPath
        pytorch_helper = Get-FileSha256 $torchScript
        powershell_driver = Get-FileSha256 $driverPath
    }
    foreach ($name in @("sura_benchmark", "pytorch_helper", "powershell_driver")) {
        if ([string]$sourceHashesBefore[$name] -ne [string]$sourceHashesAfter[$name]) {
            $validationFailures.Add("benchmark source changed during execution: $name")
        }
    }

    $reportedCublasLibrary = if ($suraOk) { [string]$sura.cublas_library } else { $null }
    $cublasHashPath = Resolve-CublasHashPath $benchmarkCublasLibrary `
        $reportedCublasLibrary $enginePath
    if ($suraOk -and [bool]$sura.cublas_available -and -not $cublasHashPath) {
        $validationWarnings.Add("loaded cuBLAS path could not be resolved for SHA-256 metadata")
    }

    $cpuName = [Environment]::GetEnvironmentVariable("PROCESSOR_IDENTIFIER")
    $gpuName = if ($suraOk) { [string]$sura.device } else { $null }
    $gpuDriver = $null
    $osCaption = [Environment]::OSVersion.VersionString
    $osBuild = [Environment]::OSVersion.Version.ToString()
    if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
        try {
            $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
            $gpu = Get-CimInstance Win32_VideoController |
                Where-Object { $_.Name -match "NVIDIA" } | Select-Object -First 1
            $os = Get-CimInstance Win32_OperatingSystem
            if ($cpu) { $cpuName = [string]$cpu.Name }
            if ($gpu) {
                if (-not $gpuName) { $gpuName = [string]$gpu.Name }
                $gpuDriver = [string]$gpu.DriverVersion
            }
            if ($os) {
                $osCaption = [string]$os.Caption
                $osBuild = [string]$os.BuildNumber
            }
        } catch {
            # Informative metadata must not alter benchmark measurements.
        }
    }

    $validationPassed = $validationFailures.Count -eq 0
    $overallStatus = if (-not $validationPassed) {
        "validation_failed"
    } elseif ($pythonRequired) {
        "ok"
    } else {
        "sura_only"
    }
    $report = [ordered]@{
        schema = $reportSchema
        generated_at_utc = [DateTime]::UtcNow.ToString("o")
        status = $overallStatus
        comparison_mode = if ($pythonRequired) { "required_pytorch" } else { "sura_only" }
        fairness = [ordered]@{
            same_host = $true
            same_gpu_device = $sameGpuDevice
            same_device_name = $sameDeviceName
            same_device_index = $sameDeviceIndex
            same_compute_capability = $sameComputeCapability
            same_device_uuid = $sameDeviceUuid
            same_pci_bus_id = $samePciBusId
            same_safetensors_input_sha256 = if ($torchOk) { [string]$torch.input_file_sha256 -eq [string]$inputFileHash } else { $null }
            deterministic_performance_seeds = @(41011, 52021, 63031)
            deterministic_correctness_seeds = @(71011, 72019, 73009)
            source_dtype = "float32"
            matrix_shape = @($MatrixSize, $MatrixSize)
            correctness_shape = @($CorrectnessSize, $CorrectnessSize)
            aligned_multiple = 16
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
            setup_and_upload_excluded = $true
            gradient_zeroing_excluded = $true
            correctness_observation_excluded = $true
            synchronization_inside_each_sample = $true
            timing_scope = "synchronized eager forward/autograd wall-clock, not a pure GEMM-kernel timer"
        }
        precision_scope = [ordered]@{
            sura = "f32 input/weight/output/gradient storage with f32, f16, or bf16 matmul compute"
            pytorch = if ($torchOk) { "native dtype input/weight/output/gradient storage for each dtype" } else { $null }
            nvidia_tf32_override_applied = "0"
            nvidia_tf32_override_previous = $previousEnvironment["NVIDIA_TF32_OVERRIDE"]
            sura_float32_math_policy = if ($suraOk) { $sura.float32_math_policy } else { $null }
            sura_policy_caveat = "cuBLAS default math mode plus NVIDIA_TF32_OVERRIDE=0; not an instruction-level IEEE-mode proof"
            pytorch_float32_matmul_policy = if ($torchOk) { $torch.float32_matmul_policy } else { $null }
            pytorch_float32_matmul_precision = if ($torchOk) { $torch.float32_matmul_precision } else { $null }
            pytorch_reduced_precision_reduction = if ($torchOk) { $torch.reduced_precision_reduction } else { $null }
            low_precision_storage_comparison_is_like_for_like = $false
            cublas_fast_counter_meaning = "successful cuBLAS FAST compute request; Tensor-Core-eligible, not proof of executed HMMA instructions"
        }
        validation = [ordered]@{
            passed = $validationPassed
            failures = @($validationFailures)
            warnings = @($validationWarnings)
            python_required = $pythonRequired
            same_gpu_device = $sameGpuDevice
            input_checksums = $checksumValidation
            sura_cases = $caseValidation
            pytorch_cases = $torchCaseValidation
            dispatch = $dispatchValidation
            fingerprint_tolerances = $fingerprintTolerances
            correctness_fingerprints = $fingerprintValidation
        }
        reproducibility = [ordered]@{
            shared_input_file_sha256 = $inputFileHash
            source_paths = [ordered]@{
                sura_benchmark = $benchmarkPath
                pytorch_helper = $torchScript
                powershell_driver = $driverPath
            }
            source_sha256_before = $sourceHashesBefore
            source_sha256_after = $sourceHashesAfter
            engine_sha256 = Get-FileSha256 $enginePath
            python_executable_sha256 = Get-FileSha256 $pythonPath
            cublas_library_sha256 = Get-FileSha256 $cublasHashPath
            cublas_library_hash_path = $cublasHashPath
            environment = [ordered]@{
                NVIDIA_TF32_OVERRIDE = "0"
                previous_NVIDIA_TF32_OVERRIDE = $previousEnvironment["NVIDIA_TF32_OVERRIDE"]
                CUDA_VISIBLE_DEVICES = [Environment]::GetEnvironmentVariable("CUDA_VISIBLE_DEVICES", "Process")
                CUBLAS_WORKSPACE_CONFIG = [Environment]::GetEnvironmentVariable("CUBLAS_WORKSPACE_CONFIG", "Process")
                SURA_CUBLAS_DISABLE = [Environment]::GetEnvironmentVariable("SURA_CUBLAS_DISABLE", "Process")
                SURA_CUDA_DEVICE = [Environment]::GetEnvironmentVariable("SURA_CUDA_DEVICE", "Process")
            }
        }
        hardware = [ordered]@{
            cpu = $cpuName
            logical_processors = [Environment]::ProcessorCount
            gpu = $gpuName
            gpu_driver = $gpuDriver
        }
        operating_system = [ordered]@{
            caption = $osCaption
            build = $osBuild
            is_64_bit = [Environment]::Is64BitOperatingSystem
            powershell_version = $PSVersionTable.PSVersion.ToString()
        }
        runtime = [ordered]@{
            engine = $enginePath
            python = $pythonPath
            python_required = $pythonRequired
            python_version = if ($torch) { $torch.python_version } else { $null }
            cublas_library = $benchmarkCublasLibrary
        }
        sura = $sura
        pytorch = $torch
        comparisons = $comparisons
    }

    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("# Sura CUDA Mixed-Compute Benchmark")
    $lines.Add("")
    $lines.Add("- Status: ``$overallStatus``")
    $lines.Add("- Comparison mode: ``$($report.comparison_mode)``")
    $lines.Add("- GPU: $(Format-MarkdownCell $gpuName)")
    $lines.Add("- Performance shape: ``$MatrixSize x $MatrixSize``")
    $lines.Add("- Correctness shape: ``$CorrectnessSize x $CorrectnessSize``")
    $lines.Add("- Samples: $Warmup warmup, $Runs measured per workload/dtype")
    $lines.Add("- GPU preflight utilization: median $(Format-MarkdownCell $gpuPreflightUtilization)%, peak $(Format-MarkdownCell $gpuPreflightPeakUtilization)% across seven one-second samples ``$($gpuPreflightSamples -join ', ')`` (public median limit $MaxPreflightGpuUtilization%; performance valid: $performanceValid)")
    $lines.Add("- Timing: synchronized eager forward/autograd wall-clock; not a pure GEMM-kernel timer")
    $lines.Add("- Shared input SHA-256: ``$inputFileHash``")
    $lines.Add("- NVIDIA_TF32_OVERRIDE: ``0`` (caller value restored after the run)")
    $lines.Add("- Ratios: Sura/PyTorch; lower is better")
    $lines.Add("- Validation: $validationPassed")
    if (-not $pythonRequired) {
        $lines.Add("- PyTorch: not requested; pass ``-Python <python.exe>`` for a required cross-framework run")
    } elseif ($torchOk) {
        $lines.Add("- PyTorch: $($torch.torch_version), f32 policy $(Format-MarkdownCell $torch.float32_matmul_policy), precision $(Format-MarkdownCell $torch.float32_matmul_precision)")
    }
    $lines.Add("")
    $lines.Add("| dtype | workload | Sura storage/output | Sura median ms | Sura p95 ms | PyTorch storage/output | PyTorch median ms | PyTorch p95 ms | Sura/PyTorch |")
    $lines.Add("|---|---|---|---:|---:|---|---:|---:|---:|")
    foreach ($dtype in @("float32", "float16", "bfloat16")) {
        $suraCase = if ($suraOk) { Get-NamedProperty $sura.cases $dtype } else { $null }
        $torchCase = if ($torchOk) { Get-NamedProperty $torch.cases $dtype } else { $null }
        $comparison = Get-NamedProperty ([pscustomobject]$comparisons) $dtype
        foreach ($workload in @("forward", "forward_backward")) {
            $label = if ($workload -eq "forward") { "forward" } else { "forward+backward" }
            $suraEntry = if ($null -ne $suraCase) { Get-NamedProperty $suraCase $workload } else { $null }
            $torchEntry = if ($null -ne $torchCase -and [string]$torchCase.status -eq "ok") { Get-NamedProperty $torchCase $workload } else { $null }
            $suraStorage = if ($null -ne $suraEntry) { "f32 / $($suraEntry.output_dtype)" } else { "UNAVAILABLE" }
            $torchStorage = if ($null -ne $torchEntry) { "$($torchCase.storage_dtype) / $($torchEntry.output_dtype)" } else { "NOT RUN" }
            $suraMedian = Format-Milliseconds $suraEntry "median_ms"
            $suraP95 = Format-Milliseconds $suraEntry "p95_ms"
            $torchMedian = Format-Milliseconds $torchEntry "median_ms"
            $torchP95 = Format-Milliseconds $torchEntry "p95_ms"
            $ratio = "-"
            if ($null -ne $comparison -and [string]$comparison.status -eq "ok") {
                $ratioProperty = if ($workload -eq "forward") { "forward_sura_over_pytorch" } else { "forward_backward_sura_over_pytorch" }
                $ratio = [Math]::Round([double](Get-NamedProperty $comparison $ratioProperty), 3)
            }
            $lines.Add("| $dtype | $label | $suraStorage | $suraMedian | $suraP95 | $torchStorage | $torchMedian | $torchP95 | $ratio |")
        }
    }

    $lines.Add("")
    $lines.Add("## Correctness hard gate")
    $lines.Add("")
    if ($pythonRequired -and $torchOk) {
        foreach ($dtype in @("float32", "float16", "bfloat16")) {
            $tolerance = $fingerprintTolerances[$dtype]
            $lines.Add("- ${dtype}: output, dInput, and dWeight sum/L1/L2/first/center/last compared; aggregate abs=$($tolerance.aggregate_absolute), sample abs=$($tolerance.sample_absolute), rel=$($tolerance.relative)")
        }
    } elseif ($pythonRequired) {
        $lines.Add("- Required PyTorch correctness comparison was unavailable or failed; this report cannot PASS. See validation failures below.")
    } else {
        $lines.Add("- Cross-framework fingerprint comparison was not requested. Sura fingerprints and storage/counter contracts were still required to be present and finite.")
    }

    $lines.Add("")
    $lines.Add("## Dispatch validation")
    $lines.Add("")
    if ($suraOk) {
        foreach ($dtype in @("float32", "float16", "bfloat16")) {
            $dispatch = Get-NamedProperty ([pscustomobject]$dispatchValidation) $dtype
            if ($dispatch) {
                $lines.Add("- ${dtype}: static FAST eligible=$($dispatch.static_fast_eligible); forward=$($dispatch.forward) (FAST/fallback=$($dispatch.forward_fast)/$($dispatch.forward_fallback)); forward+backward=$($dispatch.forward_backward) (FAST/fallback=$($dispatch.forward_backward_fast)/$($dispatch.forward_backward_fallback)); correctness=$($dispatch.correctness) (FAST/fallback=$($dispatch.correctness_fast)/$($dispatch.correctness_fallback))")
            }
        }
    }
    foreach ($warning in $validationWarnings) {
        $lines.Add("- Warning: $(Format-MarkdownCell $warning)")
    }

    if ($validationFailures.Count -gt 0) {
        $lines.Add("")
        $lines.Add("## Validation failures")
        $lines.Add("")
        foreach ($failure in $validationFailures) {
            $lines.Add("- $(Format-MarkdownCell $failure)")
        }
    }

    $lines.Add("")
    $lines.Add("## Reproducibility")
    $lines.Add("")
    $lines.Add("- Engine SHA-256: ``$($report.reproducibility.engine_sha256)``")
    $lines.Add("- Sura benchmark SHA-256: ``$($sourceHashesAfter.sura_benchmark)``")
    $lines.Add("- PyTorch helper SHA-256: ``$($sourceHashesAfter.pytorch_helper)``")
    $lines.Add("- PowerShell driver SHA-256: ``$($sourceHashesAfter.powershell_driver)``")
    $lines.Add("- cuBLAS DLL SHA-256: ``$(Format-MarkdownCell $report.reproducibility.cublas_library_sha256)``")
    $lines.Add("- Python executable SHA-256: ``$(Format-MarkdownCell $report.reproducibility.python_executable_sha256)``")

    $lines.Add("")
    $lines.Add("## Interpretation")
    $lines.Add("")
    $lines.Add("Sura's float16 and bfloat16 rows are **compute-only mixed precision**: inputs, weights, outputs, and gradients remain float32 (4 bytes/element). They do not show 2-byte VRAM or bandwidth savings. PyTorch's low-precision rows use native float16/bfloat16 storage and output when the required comparison mode is enabled, so low-precision storage semantics are not identical.")
    $lines.Add("")
    $lines.Add("Every measured sample uses host wall-clock timing around synchronized eager framework work. It includes dispatch, allocation, graph construction/traversal, and the final synchronization. It is not an isolated CUDA-event GEMM-kernel measurement.")
    $lines.Add("")
    $lines.Add("``NVIDIA_TF32_OVERRIDE=0`` is applied to both child processes and PyTorch additionally requests highest/IEEE float32 policy. Sura uses ``cublasSgemm_v2`` with the cuBLAS handle's default math mode. This controls TF32 acceleration but is not instruction-level proof that the two libraries execute identical IEEE instruction sequences.")
    $lines.Add("")
    $lines.Add("``cublas_fast_matmul_launches`` proves a successful FAST compute request, not HMMA execution. A PTX fallback is legal when the architecture, symbol, or cuBLAS operation does not support FAST compute and is reported separately. Nsight Compute is required for instruction-level proof.")

    $jsonText = $report | ConvertTo-Json -Depth 30
    $markdownText = ($lines -join [Environment]::NewLine) + [Environment]::NewLine
    $jsonTemp = $null
    $markdownTemp = $null
    try {
        $jsonTemp = New-ValidatedTempFile $JsonOut $jsonText "json"
        $markdownTemp = New-ValidatedTempFile $MarkdownOut $markdownText "markdown"
        Publish-AtomicTempFile $jsonTemp $JsonOut
        $jsonTemp = $null
        Publish-AtomicTempFile $markdownTemp $MarkdownOut
        $markdownTemp = $null
    } finally {
        if ($jsonTemp -and (Test-Path -LiteralPath $jsonTemp -PathType Leaf)) {
            Remove-Item -LiteralPath $jsonTemp -Force
        }
        if ($markdownTemp -and (Test-Path -LiteralPath $markdownTemp -PathType Leaf)) {
            Remove-Item -LiteralPath $markdownTemp -Force
        }
    }

    if (-not $validationPassed) {
        throw "mixed-compute benchmark validation failed:`n$($validationFailures -join [Environment]::NewLine)"
    }
    if ($pythonRequired) {
        Write-Host "sura_mixed_compute_benchmark: PASS"
    } else {
        Write-Host "sura_mixed_compute_benchmark: SURA_ONLY (PyTorch not requested)"
    }
    Write-Host "json: $JsonOut"
    Write-Host "markdown: $MarkdownOut"
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousEnvironment[$name], "Process")
    }
    $resolvedInput = [IO.Path]::GetFullPath($inputPath)
    if ($resolvedInput.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedInput).StartsWith("sura-mixed-compute-bench-") -and
        (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) {
        Remove-Item -LiteralPath $resolvedInput -Force
    }
}
